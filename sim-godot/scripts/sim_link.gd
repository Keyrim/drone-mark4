class_name SimLink
extends Node

## UDP link with the flight process.
##
## One sensor packet is sent per physics tick to the flight process, which
## answers with an actuator packet to the address the sensor packet came from.
## A single unconnected socket is therefore enough: the local port is picked by
## the operating system unless local_port says otherwise.
##
## The wire layout is defined by protocol/include/protocol/sim_link.hpp,
## mirrored by the constants in protocol.gd (the single GDScript copy):
##
##   sensor   (45 bytes): u8 version, u8 type, u64 timestamp_us,
##                        3x f32 gyro [rad/s], 3x f32 accel [m/s^2],
##                        f32 baro [Pa], u8 kill switch, f32 throttle,
##                        u8 arm switch, u8 reset count
##   actuator (26 bytes): u8 version, u8 type, u64 echoed timestamp_us,
##                        4x f32 motor commands in [0, 1]
##
## Anything with another size, version or type byte is dropped. The echoed
## timestamp identifies the sensor packet a reply answers: in lockstep mode
## the tick waits for the reply to the exact packet it sent, and resends it
## when the wait times out (UDP may drop packets, the handshake may not).
##
## Vectors on the wire use the drone body frame of the protocol (x forward,
## y left, z up - the accelerometer reads +1 g on z at rest), remapped here
## from the Godot body axes (y up, -z forward, x right).

## Axis remap from the Godot body frame to the drone body frame: columns are
## the drone coordinates of the Godot x, y and z axes.
const GODOT_TO_DRONE := Basis(Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0))

const MOTOR_COUNT := 4

## Offsets inside the actuator packet: version + type, then the echo.
const ACTUATOR_ECHO_OFFSET := 2
const ACTUATOR_MOTOR_OFFSET := 10
const FLOAT_SIZE := 4

## Sleep between two polls while waiting for a lockstep reply.
const LOCKSTEP_POLL_INTERVAL_US := 20

## Sensor packet resends before a lockstep tick gives up.
const LOCKSTEP_RESEND_LIMIT := 20

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
## Number of sensor packets resent while waiting for their echo.
var lockstep_resends: int = 0

var _socket := PacketPeerUDP.new()
var _buffer := StreamPeerBuffer.new()
var _ready_to_send: bool = false
var _pending_echo_us: int = -1
var _last_payload := PackedByteArray()


func _ready() -> void:
	flight_port = SimArgs.get_port("flight-port", flight_port)
	if SimArgs.has_flag("lockstep"):
		lockstep = true
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
## @param arm_switch true when the drone is armed for an autonomous throw flight.
## @param reset_count world reset counter, tells the flight process to restart.
func exchange(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	kill_switch: bool,
	throttle: float,
	arm_switch: bool,
	reset_count: int
) -> void:
	if not _ready_to_send:
		return
	_send_sensor_packet(
		timestamp_us, gyro_rad_s, accel_mps2, baro_pa, kill_switch, throttle, arm_switch, reset_count
	)
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
	throttle: float,
	arm_switch: bool,
	reset_count: int
) -> void:
	var gyro_drone := GODOT_TO_DRONE * gyro_rad_s
	var accel_drone := GODOT_TO_DRONE * accel_mps2
	_buffer.clear()
	_buffer.put_u8(Protocol.VERSION)
	_buffer.put_u8(Protocol.TYPE_SIM_SENSOR)
	_buffer.put_u64(timestamp_us)
	_buffer.put_float(gyro_drone.x)
	_buffer.put_float(gyro_drone.y)
	_buffer.put_float(gyro_drone.z)
	_buffer.put_float(accel_drone.x)
	_buffer.put_float(accel_drone.y)
	_buffer.put_float(accel_drone.z)
	_buffer.put_float(baro_pa)
	_buffer.put_u8(1 if kill_switch else 0)
	_buffer.put_float(throttle)
	_buffer.put_u8(1 if arm_switch else 0)
	_buffer.put_u8(reset_count)

	var payload := _buffer.data_array
	if payload.size() != Protocol.SIM_SENSOR_PACKET_SIZE:
		push_error("sim link: built a %d byte sensor packet" % payload.size())
		return
	_pending_echo_us = timestamp_us
	_last_payload = payload
	if _socket.put_packet(payload) == OK:
		packets_sent += 1


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


## Decode every pending reply, return how many answered the pending packet.
func _drain_replies() -> int:
	var matched := 0
	while _socket.get_available_packet_count() > 0:
		var echo := _decode_actuator_packet(_socket.get_packet())
		if echo >= 0:
			packets_received += 1
			if echo == _pending_echo_us:
				matched += 1
		else:
			packets_dropped += 1
	return matched


## @return the echoed timestamp of a valid packet, -1 when rejected.
func _decode_actuator_packet(payload: PackedByteArray) -> int:
	if payload.size() != Protocol.SIM_ACTUATOR_PACKET_SIZE:
		return -1
	if not Protocol.has_header(payload, Protocol.TYPE_SIM_ACTUATOR):
		return -1
	var decoded := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
	for index: int in MOTOR_COUNT:
		var value := payload.decode_float(ACTUATOR_MOTOR_OFFSET + index * FLOAT_SIZE)
		if not is_finite(value):
			return -1
		decoded[index] = clampf(value, 0.0, 1.0)
	motor_commands = decoded
	return payload.decode_u64(ACTUATOR_ECHO_OFFSET)
