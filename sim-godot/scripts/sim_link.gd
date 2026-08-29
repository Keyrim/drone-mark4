class_name SimLink
extends Node

## UDP link with the flight process, speaking the project wire.
##
## One SimSensor envelope is sent per physics tick to the flight process,
## which answers with a SimActuator envelope to the address the sensor came
## from. A single unconnected socket is therefore enough: the local port is
## picked by the operating system unless local_port says otherwise.
##
## The wire is software/components/protocol/mark4.proto; the codec used here
## (scripts/gen/mark4.gd) is generated from it by the desktop build (target
## proto_gd), never edited. The plant is not a transport node: envelopes
## travel bare in the datagrams of this link.
##
## Sensors only: the pilot state is not a sensor reading and does not
## travel here, and this project does not hold one. It reaches the flight
## process out-of-band as an Rc envelope, the same path the real board is
## flown through. The plant's exact state (PlantTruth) rides inside every
## sensor envelope, and the flight process forwards it inside its telemetry
## so the ground tools compare estimate and truth sample by sample.
##
## Anything that does not decode as a SimActuator or a SimScenario is
## dropped. The echoed timestamp identifies the sensor envelope a reply
## answers: in lockstep mode the tick waits for the reply to the exact
## envelope it sent, and resends it when the wait times out (UDP may drop
## packets, the handshake may not).
##
## A scenario arrives as its own SimScenario envelope on this link, whenever
## the flight process received one from the ground. A scenario is taken once
## per change of its sequence, and the repeats are ignored.
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

@export_group("Endpoint")
## Address of the flight process (drone_sim).
@export var flight_host: String = "127.0.0.1"
## UDP port the flight process listens on, SIM_LINK_PORT in protocol/ports.hpp.
@export var flight_port: int = 47800
## Local UDP port, 0 lets the operating system pick an ephemeral one.
@export var local_port: int = 0

@export_group("Lockstep")
## When true the physics tick waits for the actuator reply before completing.
## This blocks the main thread and is meant for deterministic runs.
@export var lockstep: bool = false
## Maximum wait before giving up and reusing the last known motor commands.
@export var lockstep_timeout_ms: float = 50.0

## Last valid motor commands received, in [0, 1].
var motor_commands := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
## Number of sensor envelopes handed to the socket.
var packets_sent: int = 0
## Number of valid actuator envelopes decoded.
var packets_received: int = 0
## Number of datagrams rejected because they did not decode.
var packets_dropped: int = 0
## Number of physics ticks that hit the lockstep timeout.
var lockstep_timeouts: int = 0
## Number of sensor envelopes resent while waiting for their echo.
var lockstep_resends: int = 0

var _socket := PacketPeerUDP.new()
var _ready_to_send: bool = false
var _pending_echo_us: int = -1
var _last_payload := PackedByteArray()
var _last_scenario_seq: int = 0
var _pending_scenario: Dictionary = {}


func _ready() -> void:
	flight_port = SimArgs.get_port("flight-port", flight_port)
	if SimArgs.has_flag("lockstep"):
		lockstep = true
	var bind_error := _socket.bind(local_port, "0.0.0.0")
	if bind_error != OK:
		push_error("sim link: cannot bind local port %d (error %d)" % [local_port, bind_error])
		return
	var destination_error := _socket.set_dest_address(flight_host, flight_port)
	if destination_error != OK:
		push_error(
			(
				"sim link: cannot resolve %s:%d (error %d)"
				% [flight_host, flight_port, destination_error]
			)
		)
		return
	_ready_to_send = true
	print(
		(
			"sim link: local port %d, flight process %s:%d, lockstep %s, wire %08x"
			% [_socket.get_local_port(), flight_host, flight_port, str(lockstep), WireHash.VALUE]
		)
	)


func _exit_tree() -> void:
	_socket.close()


## Send one sensor envelope and collect the replies.
##
## In free running mode the pending replies are drained without blocking, so
## the motor commands applied on this tick usually answer the previous one. In
## lockstep mode the call blocks until a reply arrives or the timeout expires.
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
	if not _ready_to_send:
		return
	_send_sensor(
		timestamp_us, gyro_rad_s, accel_mps2, baro_pa, reset_count, body_basis, position, velocity
	)
	if lockstep:
		if not _wait_for_reply():
			lockstep_timeouts += 1
	else:
		_drain_replies()


## Take the scenario received since the last call, if any.
##
## @return the decoded scenario, or an empty dictionary when nothing is pending.
func take_scenario() -> Dictionary:
	var pending := _pending_scenario
	_pending_scenario = {}
	return pending


## Format the last motor commands for the overlay.
func motor_commands_text() -> String:
	var parts := PackedStringArray()
	for index: int in MOTOR_COUNT:
		parts.append("%.3f" % motor_commands[index])
	return " ".join(parts)


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
	_last_payload = payload
	if _socket.put_packet(payload) == OK:
		packets_sent += 1


## Append the three components of a vector to a repeated float field.
func _add_vector(add: Callable, vector: Vector3) -> void:
	add.call(vector.x)
	add.call(vector.y)
	add.call(vector.z)


func _wait_for_reply() -> bool:
	var resends := 0
	var deadline_us := Time.get_ticks_usec() + int(lockstep_timeout_ms * 1000.0)
	while true:
		if _drain_replies() > 0:
			return true
		if Time.get_ticks_usec() >= deadline_us:
			if resends >= LOCKSTEP_RESEND_LIMIT:
				return false
			resends += 1
			lockstep_resends += 1
			var _sent := _socket.put_packet(_last_payload)
			deadline_us = Time.get_ticks_usec() + int(lockstep_timeout_ms * 1000.0)
		else:
			OS.delay_usec(LOCKSTEP_POLL_INTERVAL_US)
	return false


## Decode every pending datagram, return how many answered the pending envelope.
func _drain_replies() -> int:
	var matched := 0
	while _socket.get_available_packet_count() > 0:
		var envelope := Mark4.Envelope.new()
		if envelope.from_bytes(_socket.get_packet()) != Mark4.PB_ERR.NO_ERRORS:
			packets_dropped += 1
			continue
		match envelope.get_body_case():
			Mark4.Envelope.BodyCase.SIM_ACTUATOR:
				var echo := _take_actuator(envelope.get_sim_actuator())
				if echo >= 0:
					packets_received += 1
					if echo == _pending_echo_us:
						matched += 1
				else:
					packets_dropped += 1
			Mark4.Envelope.BodyCase.SIM_SCENARIO:
				_take_scenario(envelope.get_sim_scenario())
			_:
				packets_dropped += 1
	return matched


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
