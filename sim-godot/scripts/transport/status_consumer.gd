class_name Mark4StatusConsumer
extends RefCounted

## The consumer side of the Status stream: this plant asks each drone it
## hosts to send it, and stops asking when the drone leaves.
##
## A drone emits its Status to the nodes that subscribed to it, and to
## nobody else: the initiator of a stream is the consumer. So the plant
## sends one StatusSubscribe {enabled: true} per drone it hosts, as a
## request (numbered, resent, given up on: Mark4Requests), and the drone
## answers with the same message as applied - enabled false when it has no
## room for one more subscriber. The stream itself is read where it is
## used, in each drone's SimLink; nothing of it passes through here.
##
## The subscription is binary and carries no parameter: what the stream
## holds and how often it comes are states of the drone, not of this plant.

const Mark4 := preload("res://scripts/gen/mark4.gd")

## First two bytes of an Envelope whose body is a StatusSubscribe: the
## varint tag of field 41, wire type 2. The answer is told from its tag
## like every payload the plant looks at, before any codec runs.
const TAG_STATUS_SUBSCRIBE: Array[int] = [0xCA, 0x02]

## A drone answered: it holds the stream, or it refused it.
signal subscription_changed(node_id: int, subscribed: bool)

## Node id -> {requested: request id, subscribed: bool}.
var entries: Dictionary = {}

var _transport: Mark4Transport = null
var _requests: Mark4Requests = null


## Bind this consumer to the transport it reads and the helper it asks on.
## @param transport transport node of this process.
## @param requests the request helper that numbers, resends and gives up.
func setup(transport: Mark4Transport, requests: Mark4Requests) -> void:
	_transport = transport
	_requests = requests
	transport.payload_received.connect(_on_payload)
	requests.request_failed.connect(_on_request_failed)


## Ask that node for its Status stream.
func subscribe(node_id: int) -> void:
	_ask(node_id, true)


## Tell that node to stop its Status stream. A node that went down needs
## nothing: it forgot its subscribers when it left.
func unsubscribe(node_id: int) -> void:
	_ask(node_id, false)


## True while that node answered that it holds the stream.
func subscribed(node_id: int) -> bool:
	var entry: Dictionary = entries.get(node_id, {})
	return not entry.is_empty() and entry["subscribed"]


func _ask(node_id: int, enabled: bool) -> void:
	var envelope := Mark4.Envelope.new()
	var body: Mark4.StatusSubscribe = envelope.new_status_subscribe()
	body.set_enabled(enabled)
	var id := _requests.request(node_id, envelope)
	var entry: Dictionary = entries.get(node_id, {"requested": 0, "subscribed": false})
	entry["requested"] = id
	entries[node_id] = entry


func _on_payload(src: int, payload: PackedByteArray) -> void:
	if (
		payload.size() < 2
		or payload[0] != TAG_STATUS_SUBSCRIBE[0]
		or payload[1] != TAG_STATUS_SUBSCRIBE[1]
	):
		return
	var envelope := Mark4.Envelope.new()
	if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		return
	if envelope.get_body_case() != Mark4.Envelope.BodyCase.STATUS_SUBSCRIBE:
		return
	var enabled: bool = envelope.get_status_subscribe().get_enabled()
	var entry: Dictionary = entries.get(src, {"requested": 0, "subscribed": false})
	entry["requested"] = 0
	entry["subscribed"] = enabled
	entries[src] = entry
	subscription_changed.emit(src, enabled)


## The helper gave up on a request. Only the subscribe this node is waiting
## for concerns this consumer: every request of this process comes out of
## the same signal.
func _on_request_failed(dst: int, id: int) -> void:
	var entry: Dictionary = entries.get(dst, {})
	if entry.is_empty() or entry["requested"] != id:
		return
	entry["requested"] = 0
	push_warning("status: node %08x never answered the subscribe, no stream from it" % dst)
