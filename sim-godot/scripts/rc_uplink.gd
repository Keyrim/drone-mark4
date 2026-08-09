class_name RcUplink
extends Node

## Streams the pilot state to the real board, through the serial bridge.
##
## The keyboard pilot (K, A, Up/Down) is the single cockpit: its state is
## packed as a SimCommandPacket RC command (protocol/include/protocol/
## commands.hpp) and sent at 10 Hz over UDP to the serial bridge
## (tools/telemetry/serial_bridge.py, RC_COMMAND_PORT), which relays it to
## the flight controller over the UART. R sends a RESET command on top of
## the stream: the bridge turns it into a board reboot, so the key that
## restarts the sim world also restarts the real board. Streaming is the
## contract: the firmware reverts to kill+disarmed after 500 ms of
## silence, so closing the simulator is itself a safe action.
##
## The port is distinct from the scenario command port this simulator
## listens on (the sim binds its port exclusively, and the ghost view use
## case runs both at once). Harmless when no bridge is listening: the
## datagrams fall on the floor.
##
## The uplink never runs headless: a headless instance is a batch
## campaign, and a campaign must not stream arm/kill states at the
## real-board bridge port a bench session may be using. Interactive
## sessions can opt out with --no-rc-uplink.

## Seconds between two packets: 10 Hz, five packets per fail-safe window.
const SEND_PERIOD_S := 0.1

@export_group("Endpoint")
## Destination of the stream, RC_COMMAND_PORT in the headers.
@export var host: String = "127.0.0.1"
@export var port: int = 47805

## Pilot whose state is streamed.
@export var pilot_path: NodePath

## Number of packets handed to the socket.
var packets_sent: int = 0

var _pilot: PilotInput
var _socket := PacketPeerUDP.new()
var _buffer := StreamPeerBuffer.new()
var _ready_to_send: bool = false
var _since_last_send: float = 0.0


func _ready() -> void:
	if DisplayServer.get_name() == "headless":
		# Batch campaigns run headless and share the machine with bench
		# sessions; their pilot state must never reach the real board.
		return
	if SimArgs.has_flag("no-rc-uplink"):
		print("rc uplink: disabled by --no-rc-uplink")
		return
	port = SimArgs.get_port("rc-port", port)
	_pilot = get_node(pilot_path) as PilotInput
	if _pilot == null:
		push_error("rc uplink: pilot_path does not point to a PilotInput")
		return
	_buffer.big_endian = false
	var destination_error := _socket.set_dest_address(host, port)
	if destination_error != OK:
		push_error(
			"rc uplink: cannot resolve %s:%d (error %d)" % [host, port, destination_error]
		)
		return
	_ready_to_send = true
	print("rc uplink: streaming pilot state to %s:%d" % [host, port])


func _exit_tree() -> void:
	_socket.close()


func _process(delta: float) -> void:
	if not _ready_to_send:
		return

	# R reboots the board too: same key as the sim world reset, relayed by
	# the bridge as a reboot command, sent immediately (not paced).
	if _pilot.take_board_reset_request():
		_send(Protocol.SIM_COMMAND_RESET)

	_since_last_send += delta
	if _since_last_send < SEND_PERIOD_S:
		return
	_since_last_send = 0.0
	_send(Protocol.SIM_COMMAND_RC)


## Pack and send one SimCommandPacket carrying the pilot state.
##
## @param command SIM_COMMAND_RC for the periodic state, SIM_COMMAND_RESET
##        on R.
func _send(command: int) -> void:
	_buffer.clear()
	_buffer.put_u8(Protocol.VERSION)
	_buffer.put_u8(Protocol.TYPE_SIM_COMMAND)
	_buffer.put_u8(command)
	_buffer.put_u8(1 if _pilot.kill_switch else 0)
	_buffer.put_u8(1 if _pilot.arm_switch else 0)
	_buffer.put_u8(Protocol.RC_MODE_MANUAL)
	_buffer.put_float(_pilot.throttle)
	for _index in range(10):
		_buffer.put_float(0.0)

	var payload := _buffer.data_array
	assert(payload.size() == Protocol.SIM_COMMAND_PACKET_SIZE)
	if _socket.put_packet(payload) == OK:
		packets_sent += 1
