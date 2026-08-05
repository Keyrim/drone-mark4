class_name SimLink
extends Node

## UDP link with the flight process.
##
## One sensor packet is sent per physics tick to the flight process, which
## answers with an actuator packet to the address the sensor packet came from.
## A single unconnected socket is therefore enough: the local port is picked by
## the operating system unless local_port says otherwise.
##
## The wire layout is defined by protocol/include/protocol/sim_link.hpp and
## protocol/include/protocol/version.hpp, which are the source of truth. Both
## packets are packed, little endian, version byte first:
##
##   sensor   (42 bytes): u8 version, u64 timestamp_us, 3x f32 gyro [rad/s],
##                        3x f32 accel [m/s^2], f32 baro [Pa],
##                        u8 kill switch, f32 throttle
##   actuator (17 bytes): u8 version, 4x f32 motor commands in [0, 1]
##
## Anything with another size or another version byte is dropped.

## Keep in sync with protocol/include/protocol/version.hpp.
const PROTOCOL_VERSION := 2

const SENSOR_PACKET_SIZE := 42
const ACTUATOR_PACKET_SIZE := 17
const MOTOR_COUNT := 4

## Offset of the first motor command inside the actuator packet.
const ACTUATOR_MOTOR_OFFSET := 1
const FLOAT_SIZE := 4

## Sleep between two polls while waiting for a lockstep reply.
const LOCKSTEP_POLL_INTERVAL_US := 20

@export_group("Endpoint")
## Address of the flight process (drone_sim).
@export var flight_host: String = "127.0.0.1"
## UDP port the flight process listens on, SIM_LINK_PORT in the headers.
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
## Number of sensor packets handed to the socket.
var packets_sent: int = 0
## Number of valid actuator packets decoded.
var packets_received: int = 0
## Number of datagrams rejected because of their size, version or content.
var packets_dropped: int = 0
## Number of physics ticks that hit the lockstep timeout.
var lockstep_timeouts: int = 0

var _socket := PacketPeerUDP.new()
var _buffer := StreamPeerBuffer.new()
var _ready_to_send: bool = false


func _ready() -> void:
	_buffer.big_endian = false
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
			"sim link: local port %d, flight process %s:%d, lockstep %s"
			% [_socket.get_local_port(), flight_host, flight_port, str(lockstep)]
		)
	)


func _exit_tree() -> void:
	_socket.close()


## Send one sensor packet and collect the actuator replies.
##
## In free running mode the pending replies are drained without blocking, so
## the motor commands applied on this tick usually answer the previous one. In
## lockstep mode the call blocks until a reply arrives or the timeout expires.
##
## @param timestamp_us simulated time of the sample [us].
## @param gyro_rad_s body angular rates [rad/s].
## @param accel_mps2 specific force in the body frame [m/s^2].
## @param baro_pa static pressure [Pa].
## @param kill_switch true when the kill switch is engaged.
## @param throttle normalized RC throttle in [0, 1].
func exchange(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	kill_switch: bool,
	throttle: float
) -> void:
	if not _ready_to_send:
		return
	_send_sensor_packet(timestamp_us, gyro_rad_s, accel_mps2, baro_pa, kill_switch, throttle)
	if lockstep:
		if not _wait_for_reply():
			lockstep_timeouts += 1
	else:
		_drain_replies()


## Format the last motor commands for the overlay.
func motor_commands_text() -> String:
	var parts := PackedStringArray()
	for index: int in MOTOR_COUNT:
		parts.append("%.3f" % motor_commands[index])
	return " ".join(parts)


func _send_sensor_packet(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	kill_switch: bool,
	throttle: float
) -> void:
	_buffer.clear()
	_buffer.put_u8(PROTOCOL_VERSION)
	_buffer.put_u64(timestamp_us)
	_buffer.put_float(gyro_rad_s.x)
	_buffer.put_float(gyro_rad_s.y)
	_buffer.put_float(gyro_rad_s.z)
	_buffer.put_float(accel_mps2.x)
	_buffer.put_float(accel_mps2.y)
	_buffer.put_float(accel_mps2.z)
	_buffer.put_float(baro_pa)
	_buffer.put_u8(1 if kill_switch else 0)
	_buffer.put_float(throttle)

	var payload := _buffer.data_array
	if payload.size() != SENSOR_PACKET_SIZE:
		push_error("sim link: built a %d byte sensor packet" % payload.size())
		return
	if _socket.put_packet(payload) == OK:
		packets_sent += 1


func _wait_for_reply() -> bool:
	var deadline_us := Time.get_ticks_usec() + int(lockstep_timeout_ms * 1000.0)
	var received := false
	while not received:
		if _drain_replies() > 0:
			received = true
		elif Time.get_ticks_usec() >= deadline_us:
			break
		else:
			OS.delay_usec(LOCKSTEP_POLL_INTERVAL_US)
	return received


func _drain_replies() -> int:
	var accepted := 0
	while _socket.get_available_packet_count() > 0:
		if _decode_actuator_packet(_socket.get_packet()):
			accepted += 1
			packets_received += 1
		else:
			packets_dropped += 1
	return accepted


func _decode_actuator_packet(payload: PackedByteArray) -> bool:
	if payload.size() != ACTUATOR_PACKET_SIZE:
		return false
	if payload.decode_u8(0) != PROTOCOL_VERSION:
		return false
	var decoded := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
	for index: int in MOTOR_COUNT:
		var value := payload.decode_float(ACTUATOR_MOTOR_OFFSET + index * FLOAT_SIZE)
		if not is_finite(value):
			return false
		decoded[index] = clampf(value, 0.0, 1.0)
	motor_commands = decoded
	return true
