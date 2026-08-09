class_name SimHand
extends Node

## Simulated human hand: pick the drone up, hold it any which way with the
## sway and tremor of a real arm, then throw it along a whipping arc.
##
## The grip is a stiff spring-damper wrench applied to the real rigid body,
## clamped to human-plausible force and torque: the physics and the sensors
## stay strictly unchanged, the hand is only a force field that vanishes at
## release. Everything the estimators see (grip vibrations, rotating thrust
## direction during the swing, tilted release) is therefore honest physics.
##
## The scripted sequence runs on simulated time (physics deltas), so batch
## pacing does not distort it. Sway phases derive from a grab counter: two
## identical grabs shake identically, which keeps campaigns reproducible.

enum State { IDLE, RAISING, HELD, SWING }

@export_group("Grip")
## Spring stiffness of the grip [N/m].
@export var grip_stiffness_n_m: float = 500.0
## Damping of the grip [N/(m/s)].
@export var grip_damping_n_s_m: float = 40.0
## Orientation stiffness [N*m/rad].
@export var grip_torque_nm_rad: float = 1.5
## Orientation damping [N*m/(rad/s)].
@export var grip_torque_damping: float = 0.12
## A human hand cannot pull harder than this [N].
@export var grip_max_force_n: float = 45.0
## Nor twist harder than this [N*m].
@export var grip_max_torque_nm: float = 2.5

@export_group("Held")
## Height the drone is held at [m].
@export var hold_height_m: float = 1.2
## Duration of the pick-up motion [s].
@export var raise_seconds: float = 0.8
## Amplitude of the slow arm sway [m].
@export var sway_amplitude_m: float = 0.025
## Amplitude of the slow orientation wobble [rad].
@export var wobble_amplitude_rad: float = 0.06
## Amplitude of the fast tremor [m]. Kept physiological: at 9 Hz the
## acceleration is amplitude * (2*pi*9)^2, so millimeters would already
## shake at a full g - a real hand tremors by a few tenths of one.
@export var tremor_amplitude_m: float = 0.0005

@export_group("Swing")
## Arc swept by the arm during the throw [rad].
@export var swing_sweep_rad: float = 1.2

var _drone: RigidBody3D = null
var _state: State = State.IDLE
var _time_s: float = 0.0
var _grab_count: int = 0

var _grab_from: Transform3D
var _hold_position: Vector3
var _held_basis: Basis

var _held_for_s: float = 0.0     ## scripted hold duration, <0 = until told
var _pending_release_velocity := Vector3.ZERO
var _pending_release_spin := Vector3.ZERO
var _pending_swing_s: float = 0.0

var _swing_pivot: Vector3
var _swing_axis: Vector3
var _swing_normal0: Vector3
var _swing_radius: float = 0.0
var _swing_duration_s: float = 0.35
var _swing_basis0: Basis
var _release_spin := Vector3.ZERO

var _prev_target_pos: Vector3
var _has_prev_target := false


func _ready() -> void:
	_drone = get_parent() as RigidBody3D


## @return true while the drone is in the hand (any phase before release).
func is_holding() -> bool:
	return _state != State.IDLE


## Forget everything about the run that just ended.
##
## The grab counter matters as much as the state does: it drives the sway
## and wobble phases, and it accumulates across runs. Without clearing it,
## two runs given the same seed would be shaken differently depending on how
## many runs had come before - which is exactly the kind of hidden history a
## reproducible campaign must not have.
func reset() -> void:
	_state = State.IDLE
	_time_s = 0.0
	_grab_count = 0
	_held_for_s = 0.0
	_has_prev_target = false
	_pending_release_velocity = Vector3.ZERO
	_pending_release_spin = Vector3.ZERO
	_pending_swing_s = 0.0
	_release_spin = Vector3.ZERO


## Pick the drone up wherever it is and hold it in the given orientation.
## @param held_basis orientation the hand settles into (Godot frame)
## @param held_for_s hold duration before an automatic swing, <0 = wait
func grab(held_basis: Basis, held_for_s: float = -1.0) -> void:
	if _drone == null or _state != State.IDLE:
		return
	_grab_count += 1
	_grab_from = _drone.global_transform
	_hold_position = Vector3(
		_grab_from.origin.x, maxf(_grab_from.origin.y, 0.05) + hold_height_m, _grab_from.origin.z
	)
	_held_basis = held_basis.orthonormalized()
	_held_for_s = held_for_s
	_state = State.RAISING
	_time_s = 0.0
	_has_prev_target = false


## Schedule the release for an automatic swing set up by grab().
## @param release_velocity_mps world velocity at release (Godot frame)
## @param release_spin_rad_s extra wrist spin at release (Godot body frame)
## @param swing_s duration of the swing
func plan_swing(release_velocity_mps: Vector3, release_spin_rad_s: Vector3, swing_s: float) -> void:
	_pending_release_velocity = release_velocity_mps
	_pending_release_spin = release_spin_rad_s
	_pending_swing_s = swing_s


## Throw now (interactive path): swing from the current held pose.
func start_swing(release_velocity_mps: Vector3, release_spin_rad_s: Vector3, swing_s: float) -> void:
	if _state != State.HELD and _state != State.RAISING:
		return
	var speed := release_velocity_mps.length()
	if speed < 0.5 or swing_s <= 0.0:
		return
	var direction := release_velocity_mps / speed

	# Arc in the vertical plane of the throw: the axis is horizontal and
	# perpendicular to the release direction. The whip profile
	# theta(t) = sweep * (t/T)^3 releases at omega_end = 3*sweep/T, and the
	# arm radius follows from omega_end * R = speed.
	var axis := Vector3.UP.cross(direction)
	if axis.length_squared() < 1e-4:
		axis = Vector3.RIGHT
	_swing_axis = axis.normalized()
	_swing_normal0 = (direction.cross(_swing_axis)).rotated(_swing_axis, -swing_sweep_rad)
	_swing_radius = speed * swing_s / (3.0 * swing_sweep_rad)
	_swing_pivot = _current_base_position() - _swing_radius * _swing_normal0
	_swing_duration_s = swing_s
	_swing_basis0 = _held_basis
	_release_spin = release_spin_rad_s
	_state = State.SWING
	_time_s = 0.0


## Advance the hand by one physics tick and apply the grip wrench.
## Called by the drone from _physics_process.
func physics_update(delta: float) -> void:
	if _state == State.IDLE or _drone == null:
		return
	_time_s += delta

	match _state:
		State.RAISING:
			if _time_s >= raise_seconds:
				_state = State.HELD
				_time_s = 0.0
		State.HELD:
			if _held_for_s >= 0.0 and _time_s >= _held_for_s:
				if _pending_swing_s > 0.0:
					start_swing(
						_pending_release_velocity, _pending_release_spin, _pending_swing_s
					)
				else:
					_held_for_s = -1.0  # hold-only scenario: keep holding
		State.SWING:
			if _time_s >= _swing_duration_s:
				_release()
				return
		_:
			pass

	var target := _target_transform()
	var target_velocity := Vector3.ZERO
	if _has_prev_target and delta > 0.0:
		target_velocity = (target.origin - _prev_target_pos) / delta
	_prev_target_pos = target.origin
	_has_prev_target = true

	_apply_grip(target, target_velocity)


func _release() -> void:
	_state = State.IDLE
	# The wrist gives its final flick as the fingers open.
	var momentum := _drone.inertia * _release_spin
	_drone.apply_torque_impulse(_drone.global_transform.basis * momentum)


func _current_base_position() -> Vector3:
	return _hold_position if _state != State.RAISING else _target_transform().origin


func _target_transform() -> Transform3D:
	match _state:
		State.RAISING:
			var progress := smoothstep(0.0, 1.0, _time_s / raise_seconds)
			var origin := _grab_from.origin.lerp(_hold_position, progress)
			var basis := _grab_from.basis.orthonormalized().slerp(_held_basis, progress)
			return Transform3D(basis, origin)
		State.HELD:
			return Transform3D(_wobbled_basis(), _hold_position + _sway_offset())
		State.SWING:
			var theta := swing_sweep_rad * pow(_time_s / _swing_duration_s, 3.0)
			var origin := _swing_pivot + _swing_radius * _swing_normal0.rotated(_swing_axis, theta)
			var basis := Basis(Quaternion(_swing_axis, theta)) * _swing_basis0
			return Transform3D(basis, origin)
		_:
			return _drone.global_transform


## Slow two-tone sway plus a faint tremor, phases seeded by the grab count
## so identical grabs are identical.
func _sway_offset() -> Vector3:
	var offset := Vector3.ZERO
	for axis: int in 3:
		var phase := fmod(float(_grab_count) * 1.618 + float(axis) * 2.1, TAU)
		offset[axis] = (
			sway_amplitude_m * sin(TAU * 0.6 * _time_s + phase)
			+ 0.4 * sway_amplitude_m * sin(TAU * 1.3 * _time_s + 2.0 * phase)
			+ tremor_amplitude_m * sin(TAU * 9.0 * _time_s + 3.0 * phase)
		)
	return offset


func _wobbled_basis() -> Basis:
	var phase := fmod(float(_grab_count) * 2.4, TAU)
	var wobble := Basis.from_euler(
		Vector3(
			wobble_amplitude_rad * sin(TAU * 0.5 * _time_s + phase),
			wobble_amplitude_rad * sin(TAU * 0.7 * _time_s + phase * 1.7),
			0.3 * wobble_amplitude_rad * sin(TAU * 8.0 * _time_s + phase * 0.6)
		)
	)
	return wobble * _held_basis


func _apply_grip(target: Transform3D, target_velocity: Vector3) -> void:
	var force := (
		grip_stiffness_n_m * (target.origin - _drone.global_position)
		+ grip_damping_n_s_m * (target_velocity - _drone.linear_velocity)
	)
	force = force.limit_length(grip_max_force_n)
	_drone.apply_central_force(force)

	# Orientation error as a rotation vector, world frame.
	var error := _drone.global_transform.basis.orthonormalized()
	var to_target := (target.basis.orthonormalized() * error.inverse()).get_rotation_quaternion()
	var rotation_vector := to_target.get_axis() * to_target.get_angle()
	if not rotation_vector.is_finite():
		rotation_vector = Vector3.ZERO
	var torque := (
		grip_torque_nm_rad * rotation_vector - grip_torque_damping * _drone.angular_velocity
	)
	torque = torque.limit_length(grip_max_torque_nm)
	_drone.apply_torque(torque)
