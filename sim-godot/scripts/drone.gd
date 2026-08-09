class_name Drone
extends RigidBody3D

## Rigid body drone: motor model, aerodynamic drag, hand throw, and the per
## tick exchange with the flight process.
##
## Body frame: y up, -z forward, x right (the Godot convention). The four
## motors sit on an X layout, numbered like a 4 in 1 ESC:
##
##       front
##   3 o       o 1
##       \   /
##        \ /          x right, -z forward
##        / \
##       /   \
##   2 o       o 0
##        rear
##
## Diagonal pairs (0, 3) and (1, 2) spin in opposite directions, so a motor
## imbalance between the two pairs yaws the airframe.
##
## Everything happens in _physics_process: read the state left by the physics
## step, sample the sensors, exchange with the flight process, then queue the
## forces for the step that follows. The single exception is the reset, which
## teleports the body and must go through _integrate_forces.
##
## The kill switch is never enforced here. It does not travel in the sensor
## packet either: RcUplink streams the pilot state out-of-band to the flight
## process command receiver, exactly the path the real board is flown
## through. The flight process is expected to answer with zero motor
## commands, which is what the simulator wants to exercise - including when
## the stream stops and its fail-safe takes over.

const MOTOR_COUNT := 4

## Distance from the center of mass to a motor axis [m].
const ARM_LENGTH_M := 0.08
## Projection of the arm on x and z for a 45 degree X layout [m].
const ARM_COMPONENT_M := ARM_LENGTH_M * 0.7071067811865476

## Motor positions in the body frame, index order as drawn above.
const MOTOR_OFFSETS: Array[Vector3] = [
	Vector3(ARM_COMPONENT_M, 0.0, ARM_COMPONENT_M),
	Vector3(ARM_COMPONENT_M, 0.0, -ARM_COMPONENT_M),
	Vector3(-ARM_COMPONENT_M, 0.0, ARM_COMPONENT_M),
	Vector3(-ARM_COMPONENT_M, 0.0, -ARM_COMPONENT_M),
]

## Sign of the yaw reaction torque of each motor, about the body y axis.
const MOTOR_YAW_SIGNS: Array[float] = [1.0, -1.0, -1.0, 1.0]

## Time constant of the first order motor lag, command to effective speed [s].
const MOTOR_TAU_S := 0.025
## Thrust of one motor at full speed [N]. Four of them give about 3.7:1.
const MAX_THRUST_N := 6.0
## Yaw reaction torque of one motor at full speed [N*m].
const MAX_YAW_TORQUE_NM := 0.012
## Linear air drag coefficient, force = -k * v [N/(m/s)].
const DRAG_COEFF_N_S_M := 0.05

const MICROSECONDS_PER_SECOND := 1000000.0

@export_group("Start pose")
## Position the drone is reset to, resting on the ground.
@export var start_position: Vector3 = Vector3(0.0, 0.03, 0.0)

@export_group("Throw")
## Velocity increment the hand gives to the drone during the throw [m/s].
@export var throw_delta_velocity_mps: Vector3 = Vector3(1.5, 6.5, 0.0)
## Angular velocity the hand gives to the drone at release [rad/s].
@export var throw_angular_velocity_rad_s: Vector3 = Vector3(2.0, 1.0, 6.0)
## Duration of the constant force phase of the throw [s]. Gravity keeps acting
## during that phase, so the release velocity is delta_velocity - g * duration.
@export var throw_duration_s: float = 0.12

var _motor_speeds := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
var _previous_velocity: Vector3 = Vector3.ZERO
var _throw_force_n: Vector3 = Vector3.ZERO
var _throw_remaining_s: float = 0.0
var _reset_pending: bool = false
## Incremented on every applied reset and sent in the sensor packet: the
## flight process discards its state, a teleport is not a physical event.
var _reset_count: int = 0
var _tick_count: int = 0
var _tick_rate_hz: float = 500.0

@onready var sensors: DroneSensors = $Sensors
@onready var sim_link: SimLink = $SimLink
@onready var sim_raw: SimRawLink = $SimRawLink
@onready var pilot: PilotInput = $PilotInput
@onready var sim_command: SimCommand = $SimCommand
@onready var hand: SimHand = $Hand


func _ready() -> void:
	# Ticks per SIMULATED second: under batch pacing the wall tick rate and
	# time_scale are both scaled, their ratio is the physical rate.
	_tick_rate_hz = float(Engine.physics_ticks_per_second) / Engine.time_scale
	_previous_velocity = linear_velocity
	if inertia.x <= 0.0 or inertia.y <= 0.0 or inertia.z <= 0.0:
		push_warning("drone: inertia is not set, the engine will infer it from the shapes")


func _physics_process(delta: float) -> void:
	if pilot.take_reset_request() or sim_command.take_reset_request():
		_reset_pending = true
	if pilot.take_grab_request() and not hand.is_holding():
		hand.grab(_random_held_basis())
	if pilot.take_throw_request():
		if hand.is_holding():
			# The hand throw: swing arc, rotating grip, tilted release.
			hand.start_swing(throw_delta_velocity_mps, throw_angular_velocity_rad_s, 0.35)
		else:
			_start_throw(throw_delta_velocity_mps, throw_angular_velocity_rad_s)
	if sim_command.take_throw_request():
		_start_throw(sim_command.throw_velocity(), sim_command.throw_angular_velocity())
	if sim_command.take_hand_throw_request():
		hand.grab(sim_command.held_basis(), sim_command.held_seconds())
		hand.plan_swing(
			sim_command.throw_velocity(),
			sim_command.throw_angular_velocity(),
			sim_command.swing_seconds()
		)
	hand.physics_update(delta)

	var body_basis := global_transform.basis
	var velocity := linear_velocity

	sensors.sample(
		body_basis, velocity, _previous_velocity, angular_velocity, global_position.y, delta
	)
	_previous_velocity = velocity

	sim_link.exchange(
		simulated_time_us(),
		sensors.gyro_rad_s,
		sensors.accel_mps2,
		sensors.baro_pa,
		_reset_count
	)
	sim_raw.publish(simulated_time_us(), body_basis, global_position, velocity)

	_update_motor_speeds(sim_link.motor_commands, delta)
	_apply_motor_forces(body_basis)
	_apply_drag(velocity)
	_apply_throw(delta)

	_tick_count += 1


func _integrate_forces(state: PhysicsDirectBodyState3D) -> void:
	# Teleporting a rigid body is only legal from here.
	if not _reset_pending:
		return
	_reset_pending = false
	_reset_count = (_reset_count + 1) % 256
	state.transform = Transform3D(Basis.IDENTITY, start_position)
	state.linear_velocity = Vector3.ZERO
	state.angular_velocity = Vector3.ZERO
	_previous_velocity = Vector3.ZERO
	_throw_remaining_s = 0.0
	_throw_force_n = Vector3.ZERO
	for index: int in MOTOR_COUNT:
		_motor_speeds[index] = 0.0


## Simulated time of the current tick [us]. It comes from the tick counter and
## the physics rate, never from a wall clock, so the stream stays deterministic
## whatever the host does with the process.
func simulated_time_us() -> int:
	return int(round(float(_tick_count) * MICROSECONDS_PER_SECOND / _tick_rate_hz))


## Simulated time of the current tick [s].
func simulated_time_s() -> float:
	return float(_tick_count) / _tick_rate_hz


## Effective motor speeds after the first order lag, in [0, 1].
func motor_speeds() -> PackedFloat32Array:
	return _motor_speeds


## Altitude above the ground plane [m].
func altitude_m() -> float:
	return global_position.y


## A plausible resting-in-hand orientation: tilted up to 60 degrees toward a
## random azimuth, with a random heading.
func _random_held_basis() -> Basis:
	var tilt := randf_range(0.0, PI / 3.0)
	var azimuth := randf_range(0.0, TAU)
	var heading := randf_range(0.0, TAU)
	var tilt_axis := Vector3(cos(azimuth), 0.0, sin(azimuth))
	return Basis(Quaternion(tilt_axis, tilt)) * Basis(Quaternion(Vector3.UP, heading))


## Queue an instant throw. The velocity increment is applied as a constant force
## over throw_duration_s, so the accelerometer sees a real thrust phase of a
## few g followed by the free fall signature once the hand lets go.
##
## @param delta_velocity_mps velocity the hand gives, Godot world frame [m/s].
## @param angular_velocity_rad_s rotation at release, Godot body frame [rad/s].
func _start_throw(delta_velocity_mps: Vector3, angular_velocity_rad_s: Vector3) -> void:
	if throw_duration_s <= 0.0:
		push_error("drone: throw_duration_s must be greater than zero")
		return
	_throw_force_n = mass * delta_velocity_mps / throw_duration_s
	_throw_remaining_s = throw_duration_s
	var body_angular_momentum := inertia * angular_velocity_rad_s
	apply_torque_impulse(global_transform.basis * body_angular_momentum)


func _update_motor_speeds(commands: PackedFloat32Array, delta: float) -> void:
	# Exact discrete solution of tau * dw/dt = u - w over one step.
	var blend := 1.0 - exp(-delta / MOTOR_TAU_S)
	for index: int in MOTOR_COUNT:
		_motor_speeds[index] += (commands[index] - _motor_speeds[index]) * blend


func _apply_motor_forces(body_basis: Basis) -> void:
	var thrust_axis := body_basis * Vector3.UP
	var yaw_torque := 0.0
	for index: int in MOTOR_COUNT:
		var speed := _motor_speeds[index]
		var squared := speed * speed
		apply_force(thrust_axis * (MAX_THRUST_N * squared), body_basis * MOTOR_OFFSETS[index])
		yaw_torque += MOTOR_YAW_SIGNS[index] * MAX_YAW_TORQUE_NM * squared
	apply_torque(thrust_axis * yaw_torque)


func _apply_drag(velocity: Vector3) -> void:
	apply_central_force(-DRAG_COEFF_N_S_M * velocity)


func _apply_throw(delta: float) -> void:
	if _throw_remaining_s <= 0.0:
		return
	apply_central_force(_throw_force_n)
	_throw_remaining_s -= delta
