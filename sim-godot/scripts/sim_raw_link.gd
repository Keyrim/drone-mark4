class_name SimRawLink
extends Node

## Broadcasts the exact simulator state over UDP so plotting tools can compare
## the estimated state (telemetry broadcast) against it, sample by sample.
##
## The wire layout is defined by protocol/include/protocol/sim_raw.hpp, which
## is the source of truth. Packed, little endian, version byte first:
##
##   sim raw (49 bytes): u8 version, u64 timestamp_us,
##                       4x f32 attitude quaternion (w x y z),
##                       3x f32 position [m], 3x f32 velocity [m/s]
##
## Everything is expressed in the drone frame convention of the protocol
## (body x forward, y left, z up; world z up), remapped from the Godot axes.

## Keep in sync with protocol/include/protocol/version.hpp.
const PROTOCOL_VERSION := 7

const PACKET_SIZE := 49

## One packet every DECIMATION physics ticks: 50 Hz at the 500 Hz tick.
const DECIMATION := 10

## Axis remap from the Godot frame to the drone frame: columns are the drone
## coordinates of the Godot x, y and z axes.
const GODOT_TO_DRONE := Basis(Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0))

@export_group("Endpoint")
## Destination of the broadcast, SIM_RAW_PORT in the headers.
@export var broadcast_address: String = "255.255.255.255"
@export var port: int = 47802

## Number of packets handed to the socket.
var packets_sent: int = 0

var _socket := PacketPeerUDP.new()
var _buffer := StreamPeerBuffer.new()
var _ready_to_send: bool = false
var _tick: int = 0


func _ready() -> void:
	_buffer.big_endian = false
	_socket.set_broadcast_enabled(true)
	var destination_error := _socket.set_dest_address(broadcast_address, port)
	if destination_error != OK:
		push_error(
			(
				"sim raw: cannot resolve %s:%d (error %d)"
				% [broadcast_address, port, destination_error]
			)
		)
		return
	_ready_to_send = true
	print("sim raw: broadcasting to %s:%d" % [broadcast_address, port])


func _exit_tree() -> void:
	_socket.close()


## Broadcast the state of the physics tick that just completed, decimated.
##
## @param timestamp_us simulated time of the tick [us].
## @param body_basis orientation of the drone, world from body, Godot axes.
## @param position world position [m], Godot axes.
## @param velocity world linear velocity [m/s], Godot axes.
func publish(timestamp_us: int, body_basis: Basis, position: Vector3, velocity: Vector3) -> void:
	_tick += 1
	if not _ready_to_send or _tick % DECIMATION != 0:
		return

	var to_drone := Quaternion(GODOT_TO_DRONE)
	var attitude := to_drone * body_basis.get_rotation_quaternion() * to_drone.inverse()
	var position_drone := GODOT_TO_DRONE * position
	var velocity_drone := GODOT_TO_DRONE * velocity

	_buffer.clear()
	_buffer.put_u8(PROTOCOL_VERSION)
	_buffer.put_u64(timestamp_us)
	_buffer.put_float(attitude.w)
	_buffer.put_float(attitude.x)
	_buffer.put_float(attitude.y)
	_buffer.put_float(attitude.z)
	_buffer.put_float(position_drone.x)
	_buffer.put_float(position_drone.y)
	_buffer.put_float(position_drone.z)
	_buffer.put_float(velocity_drone.x)
	_buffer.put_float(velocity_drone.y)
	_buffer.put_float(velocity_drone.z)

	var payload := _buffer.data_array
	if payload.size() != PACKET_SIZE:
		push_error("sim raw: built a %d byte packet" % payload.size())
		return
	if _socket.put_packet(payload) == OK:
		packets_sent += 1
