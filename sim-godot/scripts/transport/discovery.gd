class_name Mark4Discovery
extends RefCounted

## GDScript port of software/components/discovery/: who is who on the wire.
##
## The transport says a node appeared, this asks it who it is: one
## IdentityRequest unicast to it as a request of Mark4Requests, whose
## policy {IDENTITY_TIMEOUT_US, IDENTITY_RETRIES} sends it again every
## 500 ms up to 5 times, and the node is left MUTE when the helper gives
## up. An Announce coming back makes the entry KNOWN and the identity
## signal carries its kind and name. The other direction is the same
## question asked of this plant: an IdentityRequest addressed to it is
## answered with its own Announce, unicast to whoever asked and numbered
## like every answer that matters. Nothing is broadcast, nothing is
## unsolicited; presence is the transport's keepalive.
##
## Nothing here reads a clock: tick(now_us) takes the instant from the
## caller, like DiscoveryDirectory::tick(), and it holds no retry of its
## own: the timeout and the count are the request helper's. Every payload
## of the LAN goes through the handler, telemetry at 500 Hz included, so
## the body is told from its tag bytes and the codec only runs on the two
## payloads that concern it.

const Mark4 := preload("res://scripts/gen/mark4.gd")

## Silence after a request before the helper sends it again [us].
const IDENTITY_TIMEOUT_US := 500_000
## Requests sent to a node before the helper gives up on it.
const IDENTITY_RETRIES := 5

## Where this stands with a node.
enum State { PENDING, KNOWN, MUTE }

## An entry became KNOWN, or its announce changed.
signal identity(node_id: int, kind: int, name: String)
## The transport expired the node and its entry is gone.
signal forgotten(node_id: int)

## Node id -> {state, kind, name, wire_hash, asked_us, requests, request}.
var entries: Dictionary = {}

var _transport: Mark4Transport = null
var _requests: Mark4Requests = null
var _self_announce: Mark4.Envelope = null
var _request: Mark4.Envelope = null


## Bind this directory to the transport it asks and answers on.
## @param transport transport node of this process.
## @param self_announce the Announce envelope this node answers with; the
##        helper numbers and encodes it once per answer.
## @param requests the request helper that numbers, resends and gives up.
func setup(
	transport: Mark4Transport, self_announce: Mark4.Envelope, requests: Mark4Requests
) -> void:
	_transport = transport
	_self_announce = self_announce
	_requests = requests
	_request = Mark4.Envelope.new()
	var _body: Mark4.IdentityRequest = _request.new_identity_request()
	transport.node_up.connect(_on_node_up)
	transport.node_down.connect(_on_node_down)
	transport.payload_received.connect(_on_payload)
	requests.request_failed.connect(_on_request_failed)


## Start the first request of every entry nobody has asked yet. Call it
## from the loop, after the transport's poll. Once a request is with the
## helper, the resends and the giving up are its business.
## @param now_us caller's monotonic instant [us].
func tick(now_us: int) -> void:
	for node_id: int in entries:
		var entry: Dictionary = entries[node_id]
		if entry["state"] == State.PENDING and entry["requests"] == 0:
			_ask(node_id, entry, now_us)


## Node kind of a node, or -1 while its identity is not known.
func kind_of(node_id: int) -> int:
	var entry: Dictionary = entries.get(node_id, {})
	if entry.is_empty() or entry["state"] != State.KNOWN:
		return -1
	return entry["kind"]


## Every node of that kind whose identity is known.
func nodes_of_kind(kind: int) -> Array[int]:
	var found: Array[int] = []
	for node_id: int in entries:
		var entry: Dictionary = entries[node_id]
		if entry["state"] == State.KNOWN and entry["kind"] == kind:
			found.append(node_id)
	return found


func _on_node_up(node_id: int) -> void:
	# No instant here: the entry waits for the next tick(), which asks a
	# PENDING entry with no request behind it at once.
	entries[node_id] = {
		"state": State.PENDING,
		"kind": -1,
		"name": "",
		"wire_hash": 0,
		"asked_us": 0,
		"requests": 0,
		"request": 0,
	}


func _on_node_down(node_id: int) -> void:
	if entries.erase(node_id):
		forgotten.emit(node_id)


func _on_payload(src: int, payload: PackedByteArray) -> void:
	if payload.size() < 2:
		return
	if (
		payload[0] == Mark4Announce.TAG_IDENTITY_REQUEST[0]
		and payload[1] == Mark4Announce.TAG_IDENTITY_REQUEST[1]
	):
		_answer(src, payload)
		return
	if payload[0] == Mark4Announce.TAG_ANNOUNCE:
		_take_announce(src, payload)


## Answer one IdentityRequest with this node's Announce, as a request of
## its own: an answer that matters is acknowledged like everything that has
## to arrive.
func _answer(src: int, payload: PackedByteArray) -> void:
	var envelope := Mark4.Envelope.new()
	if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		return
	if envelope.get_body_case() != Mark4.Envelope.BodyCase.IDENTITY_REQUEST:
		return
	var _id := _requests.request(src, _self_announce)


## Store an Announce as the identity of the node it came from.
func _take_announce(src: int, payload: PackedByteArray) -> void:
	var envelope := Mark4.Envelope.new()
	if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		return
	if envelope.get_body_case() != Mark4.Envelope.BodyCase.ANNOUNCE:
		return
	var announce: Mark4.Announce = envelope.get_announce()
	var kind: int = announce.get_kind()
	var name: String = announce.get_name()
	var wire_hash: int = announce.get_wire_hash()
	var entry: Dictionary = entries.get(src, {})
	if entry.is_empty():
		# The transport learnt the node before this directory existed: the
		# answer makes the entry.
		entry = {
			"state": State.PENDING,
			"kind": -1,
			"name": "",
			"wire_hash": 0,
			"asked_us": 0,
			"requests": 0,
			"request": 0,
		}
		entries[src] = entry
	var changed: bool = (
		entry["state"] != State.KNOWN
		or entry["kind"] != kind
		or entry["name"] != name
		or entry["wire_hash"] != wire_hash
	)
	# A MUTE node that answers late is KNOWN like any other.
	entry["state"] = State.KNOWN
	entry["kind"] = kind
	entry["name"] = name
	entry["wire_hash"] = wire_hash
	if changed:
		identity.emit(src, kind, name)


## Start one IdentityRequest on the helper. A refusal (the transport does
## not hold the node yet) leaves the entry untouched and the next tick asks
## again.
func _ask(node_id: int, entry: Dictionary, now_us: int) -> void:
	var id := _requests.request(node_id, _request, IDENTITY_TIMEOUT_US, IDENTITY_RETRIES)
	if id == 0:
		return
	entry["asked_us"] = now_us
	entry["requests"] = 1
	entry["request"] = id


## The helper gave up on a request. The id is checked because every request
## of this process comes out of the same signal: only the one this entry is
## waiting for leaves a still PENDING node MUTE.
func _on_request_failed(dst: int, id: int) -> void:
	var entry: Dictionary = entries.get(dst, {})
	if entry.is_empty() or entry["state"] != State.PENDING or entry["request"] != id:
		# Answered in the meantime, gone, or another request of this node.
		return
	entry["state"] = State.MUTE
