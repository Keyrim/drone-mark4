class_name Mark4Requests
extends RefCounted

## GDScript port of the request side of software/components/messaging/:
## the one place this node numbers, resends and acknowledges.
##
## The transport says who is reachable, not whether a message arrived, and
## UDP loses some. So a message that has to arrive travels as a request: it
## carries a request_id of this node's own counter (field 100 of the
## Envelope, never 0), it is kept encoded and sent again every period_us up
## to retries sends, and the destination's RequestAck carrying the same id
## completes it. Out of sends, the request is dropped and reported on
## request_failed; a node that goes down takes its pending requests with
## it, each reported the same way. At least once: a request whose
## acknowledgement was lost is applied twice, so only an idempotent state
## travels this way. Stream data (Status, the lockstep frames) goes through
## the transport's send() alone: a sample is only worth its own instant.
##
## The other direction is automatic: every payload this node receives that
## carries a non-zero id is acknowledged at once, whether or not anything
## here reads it. That answer is built from the payload's first bytes, so it
## does not depend on the order the listeners of payload_received were
## connected in, and it costs no decode: peek_request_id() reads the two
## varints of the header and nothing else, on every payload including the
## 500 Hz lockstep answers.
##
## Nothing here reads a clock: tick(now_us) takes the instant from the
## caller, like Messenger::tick().

const Mark4 := preload("res://scripts/gen/mark4.gd")

## Silence after a send before the next one, unless the caller says
## otherwise [us].
const DEFAULT_PERIOD_US := 500_000
## Sends of one request before it is given up on, the first one included,
## unless the caller says otherwise.
const DEFAULT_RETRIES := 5

## First two bytes of an Envelope whose body is a RequestAck: the varint
## tag of field 40, wire type 2.
const TAG_ACK: Array[int] = [0xC2, 0x02]
## Varint tag of field 100 of the Envelope, the request id, which follows
## the body: every encoder of the project writes the fields in ascending
## order.
const TAG_REQUEST_ID: Array[int] = [0xA0, 0x06]
## Bits a varint may carry before it is taken as malformed, as in SimCodec.
const MAX_VARINT_SHIFT := 64

## One request was acknowledged by its destination.
signal request_done(dst: int, id: int)
## One request ran out of sends, or its destination went down.
signal request_failed(dst: int, id: int)

## Requests started.
var requests: int = 0
## Requests sent again because no acknowledgement came.
var resent: int = 0
## Requests given up on.
var failed: int = 0
## Acknowledgements this node sent, one per numbered payload it received.
var acked: int = 0
## Requests acknowledged by their destination.
var completed: int = 0
## Acknowledgements matching no pending request: one given up on already,
## or one answered twice.
var unmatched_acks: int = 0

var _transport: Mark4Transport = null
## Destination node id -> request id -> {bytes, period_us, retries, sends,
## sent_us}.
var _pending: Dictionary = {}
var _next_id: int = 0
## Instant of the last tick, what a request started outside one is timed
## from: the helper has no clock of its own.
var _now_us: int = 0


## Bind this helper to the transport it numbers and acknowledges on.
## @param transport transport node of this process.
func setup(transport: Mark4Transport) -> void:
	_transport = transport
	transport.payload_received.connect(_on_payload)
	transport.node_down.connect(_on_node_down)


## Send one message as a request: numbered, kept and resent until the
## destination acknowledges it.
## @param dst node to reach; one the transport does not know is refused,
##        the caller acts on node_up instead.
## @param envelope Mark4.Envelope with its body set; its request_id is
##        written here and the bytes are taken once.
## @param period_us silence before the next send [us].
## @param retries sends before giving up, the first one included.
## @return the request id, 0 when the request was refused.
func request(
	dst: int, envelope, period_us: int = DEFAULT_PERIOD_US, retries: int = DEFAULT_RETRIES
) -> int:
	if _transport == null or not _transport.nodes.has(dst):
		return 0
	var id := _new_id()
	envelope.set_request_id(id)
	var bytes: PackedByteArray = envelope.to_bytes()
	if bytes.is_empty():
		return 0
	if not _pending.has(dst):
		_pending[dst] = {}
	var by_id: Dictionary = _pending[dst]
	by_id[id] = {
		"bytes": bytes,
		"period_us": period_us,
		"retries": retries,
		"sends": 1,
		"sent_us": _now_us,
	}
	requests += 1
	# A first send the transport refuses is kept all the same: the retry is
	# exactly what covers it.
	var _sent := _transport.send(dst, bytes)
	return id


## Resend what went unanswered and give up on what is past its policy.
## Call it from the loop, after the transport's poll.
## @param now_us caller's monotonic instant [us].
func tick(now_us: int) -> void:
	_now_us = now_us
	for dst: int in _pending.keys():
		var by_id: Dictionary = _pending[dst]
		for id: int in by_id.keys():
			var entry: Dictionary = by_id[id]
			if now_us - entry["sent_us"] < entry["period_us"]:
				continue
			if entry["sends"] < entry["retries"]:
				entry["sends"] += 1
				entry["sent_us"] = now_us
				resent += 1
				var _sent := _transport.send(dst, entry["bytes"])
				continue
			# Out of sends: the destination is there for the transport and
			# deaf to this message. Whoever asked decides what that means.
			by_id.erase(id)
			failed += 1
			request_failed.emit(dst, id)
		if by_id.is_empty():
			_pending.erase(dst)


## Number of requests waiting for their acknowledgement.
func pending() -> int:
	var count := 0
	for dst: int in _pending:
		count += (_pending[dst] as Dictionary).size()
	return count


## The request id carried by one payload, 0 when it carries none.
##
## An Envelope on the wire is one length-delimited oneof field (tag varint,
## length varint, body bytes), optionally followed by field 100. This walks
## those two varints, skips the body and reads the id: no allocation, no
## codec, on every payload of the LAN.
static func peek_request_id(payload: PackedByteArray) -> int:
	var size := payload.size()
	# The tag of the body, whose last byte is the one below 0x80.
	var at := 0
	while at < size and payload[at] >= 0x80:
		at += 1
	at += 1
	var length := 0
	var shift := 0
	var complete := false
	while at < size and shift < MAX_VARINT_SHIFT:
		var byte := payload[at]
		at += 1
		length |= (byte & 0x7F) << shift
		if byte < 0x80:
			complete = true
			break
		shift += 7
	if not complete:
		return 0
	at += length
	if (
		at + 2 > size
		or payload[at] != TAG_REQUEST_ID[0]
		or payload[at + 1] != TAG_REQUEST_ID[1]
	):
		return 0
	at += 2
	var id := 0
	shift = 0
	while at < size and shift < MAX_VARINT_SHIFT:
		var byte := payload[at]
		at += 1
		id |= (byte & 0x7F) << shift
		if byte < 0x80:
			return id
		shift += 7
	return 0


func _on_payload(src: int, payload: PackedByteArray) -> void:
	var id := peek_request_id(payload)
	if (
		payload.size() >= 2
		and payload[0] == TAG_ACK[0]
		and payload[1] == TAG_ACK[1]
	):
		_complete(src, id)
		return
	if id != 0:
		_acknowledge(src, id)


## A node went down: nothing addressed to it can still arrive, so its
## requests are given up on at once, whatever their policy had left.
func _on_node_down(node_id: int) -> void:
	var by_id: Dictionary = _pending.get(node_id, {})
	_pending.erase(node_id)
	for id: int in by_id.keys():
		failed += 1
		request_failed.emit(node_id, id)


## Drop the pending request one acknowledgement answers.
func _complete(src: int, id: int) -> void:
	var by_id: Dictionary = _pending.get(src, {})
	if not by_id.has(id):
		unmatched_acks += 1
		return
	by_id.erase(id)
	if by_id.is_empty():
		_pending.erase(src)
	completed += 1
	request_done.emit(src, id)


## Answer one numbered payload with a RequestAck carrying its id. It says
## the message arrived, and nothing of what was done with it, so it leaves
## before anything here or anywhere else reads the payload. Rare enough to
## go through the generated codec.
func _acknowledge(dst: int, id: int) -> void:
	var envelope := Mark4.Envelope.new()
	var _body: Mark4.RequestAck = envelope.new_ack()
	envelope.set_request_id(id)
	if _transport.send(dst, envelope.to_bytes()):
		acked += 1


## @return the next request id of this node: a counter of its own, never 0,
##         so a request is the pair (node, id).
func _new_id() -> int:
	_next_id = (_next_id + 1) & 0xFFFFFFFF
	if _next_id == 0:
		_next_id = 1
	return _next_id
