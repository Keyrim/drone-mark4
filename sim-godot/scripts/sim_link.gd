class_name SimLink
extends Node

## Link between one virtual drone and its flight process, over the transport.
##
## One SimSensor envelope is sent per physics tick as a unicast frame to the
## drone_sim node this drone belongs to, which answers with a SimActuator
## envelope to this plant's node. The DroneManager owns the transport and
## dispatches every payload to the drone whose node sent it (receive()).
##
## The wire is software/components/protocol/mark4.proto; the codec used here
## (scripts/gen/mark4.gd) is generated from it by the desktop build (target
## proto_gd), never edited.
##
## Sensors only: the pilot state is not a sensor reading and does not
## travel here, and this project does not hold one. It reaches the flight
## process out-of-band as an Rc envelope, the same path the real board is
## flown through. The plant's exact state (PlantTruth) rides inside every
## sensor envelope, and the flight process forwards it inside its status
## so the ground tools compare estimate and truth sample by sample.
##
## The flight process also broadcasts its Status every few frames: the
## link reads it too, so the plant knows the phase, the throw count, the
## sensor validity flags and the RC link state of the drone it hosts. That
## is display only: nothing here acts on it, the plant is not the cockpit.
## Anything else the flight process emits (log lines, telemetry) is
## ignored without running the codec, and a payload that should decode but
## does not is dropped. The echoed timestamp identifies the sensor envelope
## a reply answers: in lockstep mode the tick waits for the reply to the
## exact envelope it sent, and resends it when the wait times out (UDP may
## drop packets, the handshake may not).
##
## A scenario arrives as its own SimScenario envelope, whenever the flight
## process received one from the ground. A scenario is taken once per
## change of its sequence, and the repeats are ignored.
##
## Vectors on the wire use the drone body frame of the protocol (x forward,
## y left, z up - the accelerometer reads +1 g on z at rest), remapped here
## from the Godot body axes (y up, -z forward, x right).

const Mark4 := preload("res://scripts/gen/mark4.gd")

## Axis remap from the Godot body frame to the drone body frame: columns are
## the drone coordinates of the Godot x, y and z axes.
const GODOT_TO_DRONE := Basis(Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0))

const MOTOR_COUNT := 4

## Axis remap from the drone frame to the Godot frame: the inverse of the
## GODOT_TO_DRONE basis used on the sensor path.
const DRONE_TO_GODOT := Basis(Vector3(0, 0, -1), Vector3(-1, 0, 0), Vector3(0, 1, 0))

## Sleep between two polls while waiting for a lockstep reply.
const LOCKSTEP_POLL_INTERVAL_US := 20

## Sensor envelope resends before a lockstep tick gives up.
const LOCKSTEP_RESEND_LIMIT := 20

@export_group("Lockstep")
## When true the physics tick waits for the actuator reply before completing.
## This blocks the main thread and is meant for deterministic runs.
@export var lockstep: bool = false
## Maximum wait before giving up and reusing the last known motor commands.
@export var lockstep_timeout_ms: float = 50.0

## Transport node of the flight process this drone talks to.
var flight_node: int = 0
## Last valid motor commands received, in [0, 1].
var motor_commands := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
## Number of sensor envelopes handed to the transport.
var packets_sent: int = 0
## Number of valid actuator envelopes decoded.
var packets_received: int = 0
## Number of payloads rejected because they did not decode.
var packets_dropped: int = 0
## Number of payloads of a body the plant does not read (logs, telemetry).
var packets_ignored: int = 0
## Number of Status reports decoded.
var status_received: int = 0
## Last reported flight phase (Mark4.FlightPhase), PHASE_IDLE before any.
var phase: int = 0
## Last reported throw state (Mark4.ThrowState).
var throw_state: int = 0
## Throws the flight process detected since it started.
var throw_count: int = 0
## Sensor validity flags of the last reported frame.
var imu_valid: bool = false
var baro_valid: bool = false
## The RC fail-safe is not active: a pilot was heard recently.
var rc_link_ok: bool = false
## Wall time the last Status arrived at [us], 0 before any.
var status_wall_us: int = 0
## Number of physics ticks that hit the lockstep timeout.
var lockstep_timeouts: int = 0
## Number of sensor envelopes resent while waiting for their echo.
var lockstep_resends: int = 0
## Wall time spent in exchange(), for the per-tick cost report [us].
var exchange_us: int = 0

var _transport: Mark4Transport = null
var _pending_echo_us: int = -1
var _matched: bool = false
var _last_payload := PackedByteArray()
var _last_scenario_seq: int = 0
var _pending_scenario: Dictionary = {}


## Bind this link to the transport and the flight process node it serves.
func setup(transport: Mark4Transport, node: int) -> void:
	_transport = transport
	flight_node = node
	if SimArgs.has_flag("lockstep"):
		lockstep = true


## Send one sensor envelope and collect the replies.
##
## In free running mode the replies were dispatched by the manager's poll
## at the beginning of the tick, so the motor commands applied on this tick
## usually answer the previous one. In lockstep mode the call blocks until
## the reply to this very envelope arrives or the timeout expires.
##
## @param timestamp_us simulated time of the sample [us].
## @param gyro_rad_s body angular rates [rad/s], Godot axes.
## @param accel_mps2 specific force in the body frame [m/s^2], Godot axes.
## @param baro_pa static pressure [Pa].
## @param reset_count world reset counter, tells the flight process to restart.
## @param body_basis exact orientation of the drone, world from body, Godot axes.
## @param position exact world position [m], Godot axes.
## @param velocity exact world linear velocity [m/s], Godot axes.
func exchange(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	reset_count: int,
	body_basis: Basis,
	position: Vector3,
	velocity: Vector3
) -> void:
	if _transport == null:
		return
	var started_us := Time.get_ticks_usec()
	_send_sensor(
		timestamp_us, gyro_rad_s, accel_mps2, baro_pa, reset_count, body_basis, position, velocity
	)
	if lockstep and not _wait_for_reply():
		lockstep_timeouts += 1
	exchange_us += Time.get_ticks_usec() - started_us


## One payload the transport delivered from this drone's flight process.
func receive(payload: PackedByteArray) -> void:
	if payload.is_empty():
		packets_dropped += 1
		return
	# One byte tells the body apart before the codec runs: the status
	# report and the rest of what the flight process broadcasts is not for
	# the plant.
	match payload[0]:
		Mark4Announce.TAG_SIM_ACTUATOR:
			var envelope := Mark4.Envelope.new()
			if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
				packets_dropped += 1
				return
			var echo := _take_actuator(envelope.get_sim_actuator())
			if echo < 0:
				packets_dropped += 1
				return
			packets_received += 1
			if echo == _pending_echo_us:
				_matched = true
		Mark4Announce.TAG_SIM_SCENARIO:
			var envelope := Mark4.Envelope.new()
			if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
				packets_dropped += 1
				return
			_take_scenario(envelope.get_sim_scenario())
		Mark4Announce.TAG_STATUS:
			var envelope := Mark4.Envelope.new()
			if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
				packets_dropped += 1
				return
			_take_status(envelope.get_status())
		Mark4Announce.TAG_ANNOUNCE:
			pass
		_:
			packets_ignored += 1


## Take the scenario received since the last call, if any.
##
## @return the decoded scenario, or an empty dictionary when nothing is pending.
func take_scenario() -> Dictionary:
	var pending := _pending_scenario
	_pending_scenario = {}
	return pending


## True while a Status has been heard within the last second.
func status_fresh() -> bool:
	return status_wall_us != 0 and Time.get_ticks_usec() - status_wall_us < 1_000_000


func _send_sensor(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	reset_count: int,
	body_basis: Basis,
	position: Vector3,
	velocity: Vector3
) -> void:
	var gyro_drone := GODOT_TO_DRONE * gyro_rad_s
	var accel_drone := GODOT_TO_DRONE * accel_mps2
	var envelope := Mark4.Envelope.new()
	var sensor: Mark4.SimSensor = envelope.new_sim_sensor()
	sensor.set_timestamp_us(timestamp_us)
	_add_vector(sensor.add_gyro_rad_s, gyro_drone)
	_add_vector(sensor.add_accel_mps2, accel_drone)
	sensor.set_baro_pa(baro_pa)
	sensor.set_reset_count(reset_count)
	sensor.set_lockstep_timeouts(lockstep_timeouts)

	# The exact state, in the drone convention (body x forward, y left, z up;
	# world z up; quaternion rotating body into world, w first).
	var to_drone := Quaternion(GODOT_TO_DRONE)
	var attitude := to_drone * body_basis.get_rotation_quaternion() * to_drone.inverse()
	var truth: Mark4.PlantTruth = sensor.new_truth()
	truth.add_attitude_quat(attitude.w)
	truth.add_attitude_quat(attitude.x)
	truth.add_attitude_quat(attitude.y)
	truth.add_attitude_quat(attitude.z)
	_add_vector(truth.add_position_m, GODOT_TO_DRONE * position)
	_add_vector(truth.add_velocity_mps, GODOT_TO_DRONE * velocity)

	var payload := envelope.to_bytes()
	_pending_echo_us = timestamp_us
	_matched = false
	_last_payload = payload
	if _transport.send(flight_node, payload):
		packets_sent += 1


## Append the three components of a vector to a repeated float field.
func _add_vector(add: Callable, vector: Vector3) -> void:
	add.call(vector.x)
	add.call(vector.y)
	add.call(vector.z)


## Pump the transport until the echo of the pending envelope arrives: the
## manager dispatches it back into receive() synchronously from poll().
func _wait_for_reply() -> bool:
	var resends := 0
	var deadline_us := Time.get_ticks_usec() + int(lockstep_timeout_ms * 1000.0)
	while true:
		_transport.poll(Time.get_ticks_usec())
		if _matched:
			return true
		if Time.get_ticks_usec() >= deadline_us:
			if resends >= LOCKSTEP_RESEND_LIMIT:
				return false
			resends += 1
			lockstep_resends += 1
			var _sent := _transport.send(flight_node, _last_payload)
			deadline_us = Time.get_ticks_usec() + int(lockstep_timeout_ms * 1000.0)
		else:
			OS.delay_usec(LOCKSTEP_POLL_INTERVAL_US)
	return false


## @return the echoed timestamp of a valid actuator message, -1 when rejected.
func _take_actuator(actuator) -> int:
	var motors: Array = actuator.get_motor()
	if motors.size() != MOTOR_COUNT:
		return -1
	var decoded := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
	for index: int in MOTOR_COUNT:
		var value: float = motors[index]
		if not is_finite(value):
			return -1
		decoded[index] = clampf(value, 0.0, 1.0)
	motor_commands = decoded
	return actuator.get_echo_timestamp_us()


## Keep what the status report says about the drone, for the overlay.
func _take_status(status) -> void:
	phase = status.get_flight_phase()
	throw_state = status.get_throw_state()
	throw_count = status.get_throw_count()
	imu_valid = status.get_imu_valid()
	baro_valid = status.get_baro_valid()
	rc_link_ok = status.get_rc_link_ok()
	status_wall_us = Time.get_ticks_usec()
	status_received += 1


## Latch a scenario, once per change of its sequence.
func _take_scenario(scenario) -> void:
	var sequence: int = scenario.get_sequence()
	if sequence == 0 or sequence == _last_scenario_seq:
		return
	_last_scenario_seq = sequence
	var tilt: float = scenario.get_held_tilt_rad()
	var azimuth: float = scenario.get_held_azimuth_rad()
	# Tilt about a horizontal axis at the given azimuth, expressed in the
	# drone world convention and remapped to the Godot axes.
	var axis_drone := Vector3(cos(azimuth), sin(azimuth), 0.0)
	var axis := (DRONE_TO_GODOT * axis_drone).normalized()
	_pending_scenario = {
		"kind": scenario.get_kind(),
		"seed": scenario.get_seed(),
		"throw_delay_us": scenario.get_throw_delay_us(),
		"velocity": DRONE_TO_GODOT * _to_vector(scenario.get_velocity_mps()),
		"angular": DRONE_TO_GODOT * _to_vector(scenario.get_angular_velocity_rad_s()),
		"held_s": scenario.get_held_seconds(),
		"held_basis": Basis(Quaternion(axis, tilt)),
		"swing_s": scenario.get_swing_seconds(),
	}


## Read a repeated float field as a vector, in the drone frame; missing
## components read as zero, which is what an absent field means on the wire.
func _to_vector(values: Array) -> Vector3:
	var vector := Vector3.ZERO
	for index: int in mini(values.size(), 3):
		vector[index] = values[index]
	return vector
