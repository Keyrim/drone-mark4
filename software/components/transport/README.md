# transport

The interface manager between the processes and boards of the project. An
application declares its physical links (one per medium: a UDP socket pair,
a UART), then calls `send(node, payload)`; the transport remembers on which
link and at which address every node was last heard and emits there. The
payload is opaque: today it is one `Envelope` of `protocol/mark4.proto`,
and the transport does not link `protocol/` (it depends on `drone_warnings`
alone). The core and
the UART link build for every preset, the F405 included, with no heap and
fixed-size tables; the UDP link needs BSD sockets (desktop, and lwIP on the
ESP32).

Adopted by every node: `drone_sim` and the `hub` over UDP, the Godot plant
(a GDScript port, see below),
the board over its UART (the firmware is a node
with one `UartLink` on USART1), and the ESP32 riding the drone
(`esp32-bridge/`), which is a relay: a node with a `UartLink` to the board
and a `UdpLink` on the WiFi LAN. The hub holds one `UdpLink`, so it relays
nothing: the board reaches it as one more node of the LAN, at the relay's
address. The mobile app (`software/mobile`, kind
`PHONE`) compiles no native code: `lib/back/transport/` is a Dart port of
this directory, rule for rule and constant for constant.

## Frame

Every frame opens with an 11-byte little-endian header (`transport/frame.hpp`):

| field | type | meaning |
|-------|------|---------|
| `src` | u32 | node that produced the payload |
| `dst` | u32 | node it is for, `0` = every node (`BROADCAST_NODE`) |
| `seq` | u16 | per-sender counter, wraps |
| `flags:hops` | u8 | low nibble (`FRAME_HOPS_MASK`): relays crossed so far; a sender writes 0, a relay adds one and drops a frame already at `MAX_HOPS` = 4. High nibble: flags, bit 7 (`FRAME_FLAG_KEEPALIVE`) marking the transport's own keepalive, the others written 0 and ignored on read |

The payload follows, at most `MAX_PAYLOAD` = 512 bytes; a frame whose
header carries the keepalive flag is the transport's own (see Presence) and
is never delivered whatever it carries, and an application `send()` of an
empty payload is refused. A frame without the flag and without a payload is
nobody's message: it is learnt from like any other frame and delivered
nowhere. That is the compatibility rule with a node built before the flag
existed, whose keepalive is the header alone: it is still heard, its boot id
is simply unknown. A medium that keeps
datagram boundaries (UDP) adds nothing; the UART link wraps the frame in
the serial framing (`transport/serial_framing.hpp`: `A5 5A len_lo len_hi
payload crc16`, CRC-16/CCITT-FALSE over the two length bytes and the
payload), whose payload is capped at `SERIAL_MAX_PAYLOAD` = 512 bytes like
`MAX_PAYLOAD`, so a whole envelope fits behind the header.

## Node ids

Transport-level `uint32_t`, self-assigned at start, never configured, never
0. A desktop process draws one from `/dev/urandom` (`randomNodeId()`);
`drone_sim --node-id N` pins one so a launcher knows which node is
which process. An embedded target folds its MCU UID or MAC through
`hashNodeId(bytes, size)` (FNV-1a): the board hashes its MCU unique id,
the ESP32 relay its WiFi MAC.

## API (`transport/transport.hpp`)

```cpp
Transport transport(nodeId, bootId);   // value member of the App
transport.nodeId(); transport.bootId(); // who it is, and which run of it
transport.addLink(udpLink);            // up to MAX_LINKS = 4, AbsLink&
transport.init();                      // false without a node id or a link
transport.send(dst, payload, size);    // dst 0 = broadcast on every link, 1..MAX_PAYLOAD bytes
transport.poll(nowUs, deliver, context); // drain, learn, deliver, relay, expire, keepalive
transport.isAlive(id); transport.findNode(id); transport.nodeCount(); transport.node(i);
transport.dropped(); transport.relayed();
transport.sent(); transport.sentBytes(); transport.refused(); // this node's own sends

class Presence final : public AbsPresenceListener // up to MAX_LISTENERS = 4
{
    using AbsPresenceListener::AbsPresenceListener; // attaches to the transport
    void onNodeUp(const Transport::Node &node) override;
    void onNodeDown(const Transport::Node &node) override;
};
Presence presence{transport};          // a member declared after the transport
```

The send-side counters describe this node's own `send()` calls and its
keepalives, and nothing else (a relayed frame is somebody else's send:
`relayed()` is for those). `sent()` counts the frames that reached every
link they were meant for and `sentBytes()` their payloads (a keepalive
adds one frame and no byte); `refused()` counts the rest: an empty
payload or one longer than `MAX_PAYLOAD`, no link declared, an unknown
destination, or a medium that would not take the frame (a full UART
ring). They are what a
composition reports as its own output health, which is why they live here
rather than in a wrapper around `send()`.

`poll()` is the only place anything happens, and `nowUs` comes from the
caller: the transport never reads a clock. Every frame received, whatever
its payload, refreshes the node table (`nodeId -> link, address,
lastSeenUs, lastSeq, received, lost, duplicates, hops, boot`, `MAX_NODES` =
32);
a payload addressed to this node or to everyone is handed to `deliver`,
and a keepalive, or a frame carrying no payload, is not. A frame whose `src` is
this node (its own broadcast coming back on a shared medium) is ignored.

A broadcast leaves on every declared link; a unicast leaves on the link
its destination was last heard on, and is refused while the destination is
unknown.

Sequence accounting per node: an exact repeat of the last sequence is a
duplicate and is dropped; a forward gap below `RESYNC_THRESHOLD` (1024) is
counted as lost frames; a larger jump is a restarted sender and counts as
nothing. The hub publishes these counters per node in its `NodeTable`
(`gateway.proto`).

## Presence

Presence is the keepalive, a frame flagged `FRAME_FLAG_KEEPALIVE` in its
header and carrying `KEEPALIVE_PAYLOAD_SIZE` = 4 bytes, the sender's **boot
id** little-endian (15 bytes in all), owned by the transport: every node
broadcasts one every `KEEPALIVE_PERIOD_US` (1 s, the first one on the first
`poll()`), and additionally unicasts one to a node the moment it first
appears, so a newcomer learns everyone at once. A keepalive is learnt from,
counted in the sequence accounting and relayed like any broadcast, and never
delivered: it carries no identity and no application sees it.

The boot id is the identity of one run of one node, drawn at random by the
composition and handed to the constructor (`randomBootId()` on desktop, the
RNG peripheral on the board, `esp_random()` on the ESP32); it is never
derived from the chip, because what makes a node id stable is exactly what
this number must not be. `Node::boot` keeps the last one heard, 0 until the
node's first keepalive: a node id is stable across reboots, so a node that
restarts faster than `NODE_EXPIRY_US` would otherwise never leave the tables
of its peers. The first keepalive of a node sets its `boot` silently; one
carrying a different, non-zero boot id is another incarnation of the same
id, and the transport fires `onNodeDown()` then `onNodeUp()` for it with its
counters reset, so every listener drops what it knew and starts over. A node
silent for `NODE_EXPIRY_US` (3 s, three missed keepalives) is forgotten
and every `AbsPresenceListener::onNodeDown()` fires; `onNodeUp()` fires
when a node is heard for the first time. A listener attaches itself to the
transport in its constructor and detaches in its destructor, so declaring
one as a member right after the transport is the whole wiring; a transport
takes at most `MAX_LISTENERS` (4) and `init()` fails past that. A listener
may `send()` from inside its callbacks.

## Relay

Always on, nothing to switch: a frame not for this node is forwarded with
`hops + 1`, and a frame that already carries `MAX_HOPS` is dropped: a
broadcast goes out on every link but the one it arrived on; a unicast goes
out on the link its destination was last heard on, unless that is the
arrival link (split horizon) or the destination is unknown (dropped). A
node with one link therefore relays nothing, which is why no switch is
needed. The duplicate drop by `(src, seq)` is what keeps a triangle of
relays from looping. `Node::hops` keeps the count the last frame from a
node carried: 0 for a direct neighbour, 1 for a node behind one relay. A
relay rebuilds the last header byte rather than incrementing it in place:
the hop count shares it with the flags, which cross unchanged, so a
keepalive is relayed as a keepalive.

The hub learns the board from its keepalives, relayed by the ESP32, at the
relay's IP and data port, and its unicasts to the board land there and are
relayed down the UART; the relay's own keepalives travel beside them, from
its own node id. `relayed()` counts the frames forwarded, one per link for
a broadcast.

## Links

- `AbsLink` (`transport/link.hpp`): `send(frame, size, address)`,
  `broadcast(frame, size)`, `receive(buffer, capacity, addressOut)`, all
  non-blocking. `LinkAddress` is a `std::variant<UartAddress, UdpAddress>`
  (an IPv4 host and port for UDP, an empty struct for a UART): the
  transport stores it per node and hands it back to the link the node was
  heard on without ever looking inside; a link reads its own alternative
  with `std::get_if` and refuses a `send()` to the other one.
- `UdpLink` (`transport/udp_link.hpp`, BSD sockets): one shared
  **discovery port** every node binds with `SO_REUSEADDR + SO_REUSEPORT`
  and only ever receives broadcasts on, and one ephemeral **data socket**
  every frame leaves from, broadcasts included, so the source port of any
  datagram is the node's unicast address. Broadcast =
  `sendto 255.255.255.255:discovery`; when the host has no route for it
  right now (an isolated container, a network that is down) the link
  falls back to `127.255.255.255`, the loopback broadcast, which Linux
  also delivers to every local listener, and tries the global address
  again on the next send; `loopbackFallback()` says it happened, and the
  application logs it (the link itself prints nothing: this library does
  not link the log library, a failed system call is a `false` from
  `init()`). `DISCOVERY_PORT` = 47820 is the one port a
  deployment must agree on; the constructor takes another one so a set of
  processes isolates itself from a live bench. `discoveryFd()` /
  `dataFd()` let a caller `poll(2)` instead of spinning. The same source
  compiles against lwIP on the ESP32 (`socket`, `bind`, `sendto`,
  `recvfrom` with `MSG_DONTWAIT | MSG_TRUNC`, `getsockname`, `close`
  through the VFS); lwIP defines `SO_REUSEPORT` but ignores it, and has no
  `getifaddrs()`, so the own-address list used for echo detection is
  filled by `addLocalHost()` there (`__has_include(<ifaddrs.h>)` picks the
  path).
- `UartLink` (`transport/uart_link.hpp`): owns no hardware, is handed an
  `AbsByteStream` (`read`, `write`) and applies the serial framing;
  resynchronizes on the sync pair after garbage or a torn frame (a torn
  frame swallows the next one up to its announced length, then the CRC
  fails and hunting resumes). Two byte streams exist: `Uart1Stream`
  (platform_stm32, the USART1 rings: `write` refuses a frame the transmit
  ring cannot hold whole, so the transport counts a drop instead of
  blocking the flight loop) and `UartStream` (`esp32-bridge/main/relay.cpp`,
  the ESP-IDF UART driver rings, same refusal rule). The tests drive it
  over an in-memory pipe (`software/tests/unit/byte_pipe.hpp`).
- `UdpLink` drops the echo of its own broadcasts (own data port, one of
  the host's addresses) before the transport sees them: a relay would
  otherwise count every frame it forwards as a duplicate of its source.

## GDScript port

`sim-godot/scripts/transport/transport.gd` (`Mark4Transport`) is the same
transport for the Godot plant: the same header, the same node table and
counters, the same keepalive and expiry rules, the `(src, seq)` duplicate
drop, no relay. Its two sockets follow the `UdpLink` layout, with one
substitution forced by the engine: Godot's `PacketPeerUDP.bind()` sets no
reuse option, so the discovery socket is a `UDPServer` (`listen()` sets
`SO_REUSEADDR`, which is enough on Linux to share the port with the
`SO_REUSEADDR + SO_REUSEPORT` sockets of the C++ nodes). The plant hosts
one virtual drone per node whose identity says `DRONE_SIM`, and the lockstep
exchange (`SimSensor`, `SimActuator`, `SimScenario`) is unicast frames
between the plant's node id and each `drone_sim`'s: `drone_sim` adopts as
its plant the first node whose `SimSensor` validates, until the transport
forgets it (`platform_sim/plant_link.hpp`). Nothing is configured, no
port is reserved. `sim-godot/scripts/transport/discovery.gd`
(`Mark4Discovery`) is the port of `discovery/` next to it: it asks every
node the transport learns who it is, with the same timeout and retries,
and answers the plant's own identity to whoever asks.
`sim-godot/tests/transport_check.gd` is the transport's ctest smoke,
`test_plant_link.cpp` exchanges frames with it from C++.

## Ports

| port | who | what |
|------|-----|------|
| udp/47820 | every transport node | discovery: the broadcast keepalives, and nothing else |
| ephemeral | every transport node | data socket: every unicast frame (streams, commands, answers, the lockstep sim link, the keepalive on first sight) |
| udp/47810 | hub | HTTP + WebSocket for the pages |

The ESP32 relay owns no port of its own: it is one more node on udp/47820
with an ephemeral data socket, and the board's UART carries transport
frames in the serial framing. It shares the address the board is seen at:
two node ids, one IP and one data port.

`drone_sim` and the firmware send telemetry, log lines, the `Status`
report and every answer (tuning, OTA) as unicasts to the node that asked
or subscribed; commands reach them as unicasts to their node. The
keepalive is the one broadcast left, and it carries the sender's boot id
and no identity: who a node is travels only as the answer to an
`IdentityRequest` (`software/components/discovery/`). The board's node id
is `hashNodeId()` of the 96-bit MCU unique
id (`boardNodeId()`), so it survives resets and reflashes.

## Open points

- No retransmission, no acknowledgement, no fragmentation: what the
  application needs it does above. The messenger
  (`software/components/messaging/`) numbers, keeps and resends what has to
  arrive and acknowledges what it receives; the OTA session has its own
  go-back-N on top of that.
- The board's transport keeps the full `MAX_NODES` = 32 table (about
  1.3 KB) although it only ever sees a handful of nodes; RAM is not tight
  on the F405 so nothing shrinks it.
