class_name RcUplink
extends Node

## Streams the pilot state to a flight process command receiver.
##
## The keyboard pilot (K, A, Up/Down) is the single cockpit: its state is
## packed as an RcCommandPacket (protocol/include/protocol/commands.hpp)
## and sent at 10 Hz over UDP to RC_COMMAND_PORT. The default destination
## is the local drone_sim, so an interactive flight exercises the exact
## RC path a real flight uses; pointing it at the serial bridge instead
## (--rc-port, or the host export) flies the real board through the same
## cockpit and the same packets.
##
## We only send. The receiving flight process is the one that binds the
## port, so nothing here ever fails because a receiver is missing: the
## datagrams simply fall on the floor. Silence means kill: the receiver
## reverts to kill+disarmed after 500 ms without a packet, so closing the
## simulator is itself a safe action, and holding a state means repeating
## it rather than sending it once.
##
## The cadence is wall-clock (_process), not simulated time, while the
## fail-safe window is measured in simulated time by the flight process.
## Above roughly time scale 5 the stream therefore looks intermittent to
## the receiver and the fail-safe flaps; interactive sessions run at 1x,
## and batch campaigns stream their own RC at a scaled period instead of
## using this node.
##
## The uplink never runs headless: a headless instance is a batch
## campaign, and a campaign must not stream arm/kill states at a port a
## bench session may be using. Interactive sessions can opt out with
## --no-rc-uplink.

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

	_since_last_send += delta
	if _since_last_send < SEND_PERIOD_S:
		return
	_since_last_send = 0.0
	_send()


## Pack and send one RcCommandPacket carrying the pilot state.
func _send() -> void:
	_buffer.clear()
	_buffer.put_u8(Protocol.VERSION)
	_buffer.put_u8(Protocol.TYPE_RC_COMMAND)
	_buffer.put_u8(1 if _pilot.kill_switch else 0)
	_buffer.put_u8(1 if _pilot.arm_switch else 0)
	_buffer.put_u8(Protocol.RC_MODE_MANUAL)
	_buffer.put_float(_pilot.throttle)

	var payload := _buffer.data_array
	assert(payload.size() == Protocol.RC_COMMAND_PACKET_SIZE)
	if _socket.put_packet(payload) == OK:
		packets_sent += 1
