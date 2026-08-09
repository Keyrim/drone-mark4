class_name SimCommand
extends Node

## UDP listener for scripted scenario commands (batch campaigns).
##
## The wire layout is defined by protocol/include/protocol/commands.hpp,
## mirrored by the constants in protocol.gd (the single GDScript copy):
## u8 version, u8 type, u8 command, u8 kill switch, u8 arm switch, u8 mode,
## f32 throttle, 3x f32 velocity [m/s], 3x f32 angular velocity [rad/s],
## then the hand throw floats. Vectors arrive in the drone frame convention
## of the protocol and are remapped to the Godot axes here.
##
## RC commands are applied to the pilot state immediately; reset and throw
## are one-shot requests the drone consumes at its next physics tick, exactly
## like the keyboard pilot: commands never touch the physics state directly.

## Axis remap from the drone frame to the Godot frame: the inverse of the
## GODOT_TO_DRONE basis used on the sensor path.
const DRONE_TO_GODOT := Basis(Vector3(0, 0, -1), Vector3(-1, 0, 0), Vector3(0, 1, 0))

## UDP port listened on, SIM_COMMAND_PORT in the headers.
@export var command_port: int = 47804

## Pilot the RC commands are applied to.
@export var pilot_path: NodePath

## Number of valid command packets applied.
var packets_received: int = 0
## Number of datagrams rejected because of their size or version.
var packets_dropped: int = 0

var _socket := PacketPeerUDP.new()
var _pilot: PilotInput = null
var _reset_requested := false
var _throw_requested := false
var _throw_velocity := Vector3.ZERO
var _throw_angular_velocity := Vector3.ZERO
var _hand_throw_requested := false
var _held_seconds: float = 0.0
var _held_basis := Basis.IDENTITY
var _swing_seconds: float = 0.0


func _ready() -> void:
	command_port = SimArgs.get_port("command-port", command_port)
	if not pilot_path.is_empty():
		_pilot = get_node_or_null(pilot_path) as PilotInput
	var error := _socket.bind(command_port, "0.0.0.0")
	if error != OK:
		push_error("sim command: cannot bind port %d (error %d)" % [command_port, error])
		return
	print("sim command: listening on udp/%d" % command_port)


func _exit_tree() -> void:
	_socket.close()


## Polled per physics tick, not per frame: under batch pacing a frame packs
## hundreds of ticks back to back, and a command waiting for the next frame
## boundary would arrive whole simulated seconds late.
func _physics_process(_delta: float) -> void:
	while _socket.get_available_packet_count() > 0:
		if _decode(_socket.get_packet()):
			packets_received += 1
		else:
			packets_dropped += 1


## Return true once per received reset command, then clear the request.
func take_reset_request() -> bool:
	var requested := _reset_requested
	_reset_requested = false
	return requested


## Return true once per received throw command, then clear the request.
func take_throw_request() -> bool:
	var requested := _throw_requested
	_throw_requested = false
	return requested


## Velocity of the last throw command, Godot world frame [m/s].
func throw_velocity() -> Vector3:
	return _throw_velocity


## Angular velocity of the last throw command, Godot body frame [rad/s].
func throw_angular_velocity() -> Vector3:
	return _throw_angular_velocity


## Return true once per received hand throw command, then clear the request.
func take_hand_throw_request() -> bool:
	var requested := _hand_throw_requested
	_hand_throw_requested = false
	return requested


## Held attitude of the last hand throw command, Godot frame.
func held_basis() -> Basis:
	return _held_basis


## Hold duration before the swing [s].
func held_seconds() -> float:
	return _held_seconds


## Swing duration, 0 = hold forever [s].
func swing_seconds() -> float:
	return _swing_seconds


func _decode(payload: PackedByteArray) -> bool:
	if payload.size() != Protocol.SIM_COMMAND_PACKET_SIZE:
		return false
	if not Protocol.has_header(payload, Protocol.TYPE_SIM_COMMAND):
		return false
	match payload.decode_u8(2):
		Protocol.SIM_COMMAND_RESET:
			_reset_requested = true
		Protocol.SIM_COMMAND_RC:
			if _pilot != null:
				_pilot.kill_switch = payload.decode_u8(3) != 0
				_pilot.arm_switch = payload.decode_u8(4) != 0
				_pilot.throttle = clampf(payload.decode_float(Protocol.SIM_COMMAND_THROTTLE_OFFSET), 0.0, 1.0)
		Protocol.SIM_COMMAND_THROW:
			_decode_throw_vectors(payload)
			_throw_requested = true
		Protocol.SIM_COMMAND_HAND_THROW:
			_decode_throw_vectors(payload)
			_held_seconds = payload.decode_float(Protocol.SIM_COMMAND_HELD_OFFSET)
			var tilt := payload.decode_float(Protocol.SIM_COMMAND_HELD_OFFSET + 4)
			var azimuth := payload.decode_float(Protocol.SIM_COMMAND_HELD_OFFSET + 8)
			_swing_seconds = payload.decode_float(Protocol.SIM_COMMAND_HELD_OFFSET + 12)
			# Tilt about a horizontal axis at the given azimuth, expressed in
			# the drone world convention and remapped to the Godot axes.
			var axis_drone := Vector3(cos(azimuth), sin(azimuth), 0.0)
			var axis := (DRONE_TO_GODOT * axis_drone).normalized()
			_held_basis = Basis(Quaternion(axis, tilt))
			_hand_throw_requested = true
		_:
			return false
	return true


func _decode_throw_vectors(payload: PackedByteArray) -> void:
	var velocity_drone := Vector3(
		payload.decode_float(Protocol.SIM_COMMAND_VELOCITY_OFFSET),
		payload.decode_float(Protocol.SIM_COMMAND_VELOCITY_OFFSET + 4),
		payload.decode_float(Protocol.SIM_COMMAND_VELOCITY_OFFSET + 8)
	)
	var angular_drone := Vector3(
		payload.decode_float(Protocol.SIM_COMMAND_ANGULAR_OFFSET),
		payload.decode_float(Protocol.SIM_COMMAND_ANGULAR_OFFSET + 4),
		payload.decode_float(Protocol.SIM_COMMAND_ANGULAR_OFFSET + 8)
	)
	_throw_velocity = DRONE_TO_GODOT * velocity_drone
	_throw_angular_velocity = DRONE_TO_GODOT * angular_drone
