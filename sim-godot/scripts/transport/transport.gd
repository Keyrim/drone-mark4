class_name Mark4Transport
extends RefCounted

## GDScript port of software/components/transport/: the plant is one node
## of the LAN like every other process of the project.
##
## Frames: an 11-byte little-endian header (src u32, dst u32, seq u16,
## flags:hops u8, the hop count on the low nibble and the keepalive flag on
## bit 7) then an opaque payload, one Envelope of mark4.proto. Every frame
## heard refreshes the node table (address, last sequence, hops, counters);
## a node silent for NODE_EXPIRY_US is forgotten. Presence is a keepalive, a
## flagged frame carrying this node's boot id: broadcast every
## KEEPALIVE_PERIOD_US and unicast once to a node the moment it first
## appears, carrying nothing for the application, whatever its size. A boot
## id that changes is another incarnation of the same node id, which leaves
## the table and comes back into it. Frames repeating the last sequence of
## their sender are duplicates and dropped. No relay: a frame for another
## node is only used to learn its sender.
##
## Sockets, as in the C++ UdpLink: one shared DISCOVERY socket every node
## of the deployment binds and receives broadcasts on, and one ephemeral
## DATA socket every frame leaves from, so the source port of any datagram
## is this node's unicast address. Godot's PacketPeerUDP.bind() sets no
## reuse option and refuses a port another process holds, so the discovery
## socket is a UDPServer (its listen() sets SO_REUSEADDR, which Linux
## honours for UDP next to the SO_REUSEADDR + SO_REUSEPORT pair the C++
## link sets): datagrams come out of it as one PacketPeerUDP per remote
## address, kept in _peers and drained every poll.
##
## Nothing here reads a clock: poll(now_us) takes the instant from the
## caller, like Transport::poll(). The hot path (one sensor frame per
## physics tick) reuses its buffers and allocates only what the codec does.

## Every node of a deployment agrees on this port (transport/udp_link.hpp).
const DISCOVERY_PORT := 47820
## Destination meaning "every node".
const BROADCAST_NODE := 0
const HEADER_SIZE := 11
const MAX_PAYLOAD := 512
## Relays a frame may cross. This node relays nothing, so it only ever
## writes hops 0 and keeps the ceiling for the record.
const MAX_HOPS := 4
## Bits of the last header byte holding the hop count.
const HOPS_MASK := 0x0F
## Flag of the last header byte marking the transport's own keepalive.
const FLAG_KEEPALIVE := 0x80
## Payload of a keepalive: the sender's boot id, little-endian u32.
const KEEPALIVE_PAYLOAD_SIZE := 4
const KEEPALIVE_PERIOD_US := 1_000_000
const NODE_EXPIRY_US := 3_000_000
## A forward jump of the sequence beyond this is a restarted sender.
const RESYNC_THRESHOLD := 1024
## Remote addresses the discovery socket may hold at once.
const MAX_PEERS := 64

## Sequence dedup and the loss counter are per node, as in the C++ table.
signal node_up(node_id: int)
signal node_down(node_id: int)
## One payload addressed to this node or to everyone.
signal payload_received(src: int, payload: PackedByteArray)

var node_id: int = 0
## Identity of this run of this node, in every keepalive it sends: a peer
## that sees it change knows this node restarted.
var boot_id: int = 0
var discovery_port: int = DISCOVERY_PORT
## Nodes heard within NODE_EXPIRY_US:
## id -> {address, port, last_seen_us, last_seq, received, lost, duplicates,
## hops, boot}.
var nodes: Dictionary = {}
## Frames dropped: shorter than a header, an empty payload handed to send(),
## or a unicast to an unknown node.
var dropped: int = 0

var _discovery := UDPServer.new()
var _data := PacketPeerUDP.new()
var _peers: Array[PacketPeerUDP] = []
var _next_seq: int = 0
var _last_keepalive_us: int = 0
var _keepalive_sent: bool = false
var _tx := PackedByteArray()
var _loopback_warned: bool = false


## @param own_id transport identity, never 0; drawn at random when 0.
## @param port shared discovery port of the deployment.
## @return true when both sockets are open.
func open(own_id: int = 0, port: int = DISCOVERY_PORT) -> bool:
	node_id = own_id
	while node_id == 0:
		node_id = randi()
	# Never derived from anything stable: what it has to say is that this
	# node restarted.
	while boot_id == 0:
		boot_id = randi()
	discovery_port = port
	_discovery.max_pending_connections = MAX_PEERS
	var error := _discovery.listen(discovery_port, "0.0.0.0")
	if error != OK:
		push_error("transport: cannot bind discovery port %d (error %d)" % [discovery_port, error])
		return false
	error = _data.bind(0, "0.0.0.0")
	if error != OK:
		push_error("transport: cannot bind the data socket (error %d)" % error)
		return false
	_data.set_broadcast_enabled(true)
	_tx.resize(HEADER_SIZE)
	print(
		(
			"transport: node %08x, discovery udp/%d, data udp/%d"
			% [node_id, discovery_port, _data.get_local_port()]
		)
	)
	return true


func close() -> void:
	_discovery.stop()
	_data.close()
	_peers.clear()
	nodes.clear()


## Send one payload. dst BROADCAST_NODE reaches every node of the LAN.
## An application always sends a message: an empty payload is refused, and
## the frame is never flagged as a keepalive, which is the transport's own.
## @return true when the frame was handed to the socket.
func send(dst: int, payload: PackedByteArray) -> bool:
	if payload.is_empty():
		dropped += 1
		return false
	if payload.size() > MAX_PAYLOAD:
		return false
	_tx.resize(HEADER_SIZE)
	_tx.encode_u32(0, node_id)
	_tx.encode_u32(4, dst)
	_tx.encode_u16(8, _next_seq)
	_tx.encode_u8(10, 0)
	_next_seq = (_next_seq + 1) & 0xFFFF
	_tx.append_array(payload)
	if dst == BROADCAST_NODE:
		return _broadcast()
	var node: Dictionary = nodes.get(dst, {})
	if node.is_empty():
		dropped += 1
		return false
	return _send_to(node["address"], node["port"])


## Drain both sockets, learn, deliver, expire, keepalive when due.
## @param now_us caller's monotonic instant [us].
func poll(now_us: int) -> void:
	while _data.get_available_packet_count() > 0:
		var frame := _data.get_packet()
		_on_frame(frame, _data.get_packet_ip(), _data.get_packet_port(), now_us)
	_discovery.poll()
	while _discovery.is_connection_available():
		_peers.append(_discovery.take_connection())
	for peer: PacketPeerUDP in _peers:
		while peer.get_available_packet_count() > 0:
			var frame := peer.get_packet()
			_on_frame(frame, peer.get_packet_ip(), peer.get_packet_port(), now_us)
	_expire(now_us)
	if not _keepalive_sent or now_us - _last_keepalive_us >= KEEPALIVE_PERIOD_US:
		_last_keepalive_us = now_us
		_keepalive_sent = true
		_send_keepalive(BROADCAST_NODE)


## True when the node has been heard within NODE_EXPIRY_US.
func is_alive(id: int) -> bool:
	return nodes.has(id)


## Send one keepalive, a flagged frame carrying this node's boot id: its
## presence, and nothing for the application on the other side.
## @param dst node to reach, BROADCAST_NODE for every node of the LAN.
func _send_keepalive(dst: int) -> void:
	_tx.resize(HEADER_SIZE + KEEPALIVE_PAYLOAD_SIZE)
	_tx.encode_u32(0, node_id)
	_tx.encode_u32(4, dst)
	_tx.encode_u16(8, _next_seq)
	_tx.encode_u8(10, FLAG_KEEPALIVE)
	_tx.encode_u32(HEADER_SIZE, boot_id)
	_next_seq = (_next_seq + 1) & 0xFFFF
	if dst == BROADCAST_NODE:
		var _broadcast_sent := _broadcast()
		return
	var node: Dictionary = nodes.get(dst, {})
	if node.is_empty():
		return
	var _unicast_sent := _send_to(node["address"], node["port"])


func _on_frame(frame: PackedByteArray, address: String, port: int, now_us: int) -> void:
	if frame.size() < HEADER_SIZE:
		dropped += 1
		return
	var src := frame.decode_u32(0)
	var dst := frame.decode_u32(4)
	var seq := frame.decode_u16(8)
	var flags := frame.decode_u8(10)
	var hops := flags & HOPS_MASK
	var keepalive := (flags & FLAG_KEEPALIVE) != 0
	if src == node_id or src == BROADCAST_NODE:
		return  # own broadcast coming back, or nobody
	var node: Dictionary = nodes.get(src, {})
	var is_new := node.is_empty()
	if is_new:
		node = {
			"address": address,
			"port": port,
			"last_seen_us": now_us,
			"last_seq": seq,
			"received": 1,
			"lost": 0,
			"duplicates": 0,
			"hops": hops,
			"boot": 0,
		}
		nodes[src] = node
	else:
		var delta: int = (seq - node["last_seq"]) & 0xFFFF
		if delta == 0:
			node["duplicates"] += 1
			node["last_seen_us"] = now_us
			return
		if delta > 1 and delta < RESYNC_THRESHOLD:
			node["lost"] += delta - 1
		node["last_seq"] = seq
		node["received"] += 1
		node["address"] = address
		node["port"] = port
		node["last_seen_us"] = now_us
		node["hops"] = hops
	var reincarnated := false
	if keepalive and frame.size() >= HEADER_SIZE + KEEPALIVE_PAYLOAD_SIZE:
		reincarnated = _on_boot_id(src, node, frame.decode_u32(HEADER_SIZE), is_new, seq, hops)
	if is_new:
		node_up.emit(src)
		# The newcomer learns this node at once instead of waiting for the
		# next periodic keepalive.
		_send_keepalive(src)
	elif reincarnated:
		node_down.emit(src)
		node_up.emit(src)
	# A flagged frame is the transport's own, whatever it carries, and a
	# frame without a payload has nothing for the application either: both
	# were learnt from above and stop here.
	if not keepalive and frame.size() > HEADER_SIZE and (dst == node_id or dst == BROADCAST_NODE):
		payload_received.emit(src, frame.slice(HEADER_SIZE))


## Read the boot id one keepalive carries. The first one is learnt
## silently; one that differs from a known incarnation is a node that
## restarted, whose entry is reset and whose caller emits node_down then
## node_up.
## @return true when the node is another incarnation of the same id.
func _on_boot_id(src: int, node: Dictionary, boot: int, is_new: bool, seq: int, hops: int) -> bool:
	if is_new or node["boot"] == 0:
		node["boot"] = boot
		return false
	if boot == 0 or boot == node["boot"]:
		return false
	node["last_seq"] = seq
	node["received"] = 1
	node["lost"] = 0
	node["duplicates"] = 0
	node["hops"] = hops
	node["boot"] = boot
	nodes[src] = node
	return true


func _expire(now_us: int) -> void:
	var gone: Array = []
	for id: int in nodes:
		if now_us - nodes[id]["last_seen_us"] >= NODE_EXPIRY_US:
			gone.append(id)
	for id: int in gone:
		nodes.erase(id)
		node_down.emit(id)
	if _peers.size() > MAX_PEERS:
		# ponytail: peers are never matched to nodes; a flood of strangers
		# is simply forgotten wholesale (close() unregisters a peer from the
		# server), which re-creates the live ones on their next datagram.
		for peer: PacketPeerUDP in _peers:
			peer.close()
		_peers.clear()


func _broadcast() -> bool:
	if _send_to("255.255.255.255", discovery_port):
		return true
	# No route for the global broadcast right now (isolated host): the
	# loopback broadcast still reaches every local listener, retried on the
	# next send like the C++ link.
	if not _loopback_warned:
		_loopback_warned = true
		push_warning("transport: no route for 255.255.255.255, broadcasting on the loopback")
	return _send_to("127.255.255.255", discovery_port)


func _send_to(address: String, port: int) -> bool:
	if _data.set_dest_address(address, port) != OK:
		return false
	return _data.put_packet(_tx) == OK
