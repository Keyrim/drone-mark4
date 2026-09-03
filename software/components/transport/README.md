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

Adopted by every node: `drone_sim` and the `hub` over UDP, the batch
campaign (`tools/batch/run_batch.py`, through the frame codec of
`tools/telemetry_wire.py`), the Godot plant (a GDScript port, see below),
the board over its UART (the firmware is a node
with one `UartLink` on USART1), and the ESP32 riding the drone
(`esp32-bridge/`), which is a relay: a node with a `UartLink` to the board
and a `UdpLink` on the WiFi LAN, `setRelay(true)`, a beacon of kind
`RELAY`, and a filter on what goes down the UART. The hub holds one
`UdpLink` and relays nothing: the board reaches it as one more node of the
LAN, at the relay's address. The mobile app (`software/mobile`, kind
`PHONE`) compiles this directory as it is with the Android NDK (bionic has
the BSD sockets; `DRONE_PLATFORM` `android` selects the POSIX sources) and
drives it from Dart through the C ABI of its `native/` shim.

## Frame

Every frame opens with an 11-byte little-endian header (`transport/frame.hpp`):

| field | type | meaning |
|-------|------|---------|
| `src` | u32 | node that produced the payload |
| `dst` | u32 | node it is for, `0` = every node (`BROADCAST_NODE`) |
| `seq` | u16 | per-sender counter, wraps |
| `hops` | u8 | relays left; a relay decrements and drops at 0 (`INITIAL_HOPS` = 4) |

The payload follows, at most `MAX_PAYLOAD` = 512 bytes. A medium that keeps
datagram boundaries (UDP) adds nothing; the UART link wraps the frame in
the serial framing (`transport/serial_framing.hpp`: `A5 5A len_lo len_hi
payload crc16`, CRC-16/CCITT-FALSE over the two length bytes and the
payload), whose payload is capped at `SERIAL_MAX_PAYLOAD` = 512 bytes like
`MAX_PAYLOAD`, so a whole envelope fits behind the header.

## Node ids

Transport-level `uint32_t`, self-assigned at start, never configured, never
0. A desktop process draws one from `/dev/urandom` (`randomNodeId()`);
`drone_sim --node-id N` pins one so a batch campaign knows which node is
which run. An embedded target folds its MCU UID or MAC through
`hashNodeId(bytes, size)` (FNV-1a): the board hashes its MCU unique id,
the ESP32 relay its WiFi MAC.

## API (`transport/transport.hpp`)

```cpp
Transport transport(nodeId);           // explicit, value member of the App
transport.addLink(udpLink);            // up to MAX_LINKS = 4, AbsLink&
transport.init();                      // false without a node id or a link
transport.setBeacon(bytes, size);      // optional, at most MAX_BEACON_SIZE = 64
transport.setRelay(true);              // off by default
transport.setRelayFilter(filter, context); // optional, relayed frames only
transport.setNodeCallbacks(onUp, onDown, context); // function pointer + context
transport.send(dst, payload, size, linkMask); // dst 0 = broadcast, mask optional
transport.poll(nowUs, deliver, context); // drain, learn, deliver, relay, expire, beacon
transport.isAlive(id); transport.findNode(id); transport.nodeCount(); transport.node(i);
transport.dropped(); transport.relayed(); transport.filtered();
transport.sent(); transport.sentBytes(); transport.refused(); // this node's own sends
```

The send-side counters describe this node's own `send()` calls, the
periodic beacon included, and nothing else (a relayed frame is somebody
else's send: `relayed()` and `filtered()` are for those). `sent()` counts
the calls that reached every link they were meant for and `sentBytes()`
their payloads; `refused()` counts the rest: a payload longer than
`MAX_PAYLOAD`, no link declared, a destination unknown or masked out, or a
medium that would not take the frame (a full UART ring). They are what a
composition reports as its own output health, which is why they live here
rather than in a wrapper around `send()`.

`poll()` is the only place anything happens, and `nowUs` comes from the
caller: the transport never reads a clock. Every frame received, whatever
its payload, refreshes the node table (`nodeId -> link, address,
lastSeenUs, lastSeq, received, lost, duplicates`, `MAX_NODES` = 32); a
payload addressed to this node or to everyone is handed to `deliver`. A
frame whose `src` is this node (its own broadcast coming back on a shared
medium) is ignored.

`send()` takes an optional `linkMask`, one bit per link index, `ALL_LINKS`
by default: the links the frame may leave on. It is how a node keeps a
chatty broadcast of its own off a slow link, the relay filter being for
relayed frames alone; the ESP32 relay sends its log lines and its module
table with the LAN bit only, while its beacon takes both links. A unicast
whose node sits on an excluded link does not go out.

Sequence accounting per node: an exact repeat of the last sequence is a
duplicate and is dropped; a forward gap below `RESYNC_THRESHOLD` (1024) is
counted as lost frames; a larger jump is a restarted sender and counts as
nothing. The hub publishes these counters per node in its `NodeTable`
(`gateway.proto`).

## Presence

`setBeacon()` registers an application payload (the `Announce` envelope of
a node) that the transport broadcasts every `BEACON_PERIOD_US`
(1 s, the first one on the first `poll()`), and additionally unicasts once
to a node the moment it first appears, so a newcomer learns everyone at
once. A node silent for `NODE_EXPIRY_US` (3 s) is forgotten and `onDown`
fires; `onUp` fires when a node is heard for the first time. The hub
beacons too, as kind `gateway`.

## Relay

Off by default. With `setRelay(true)`, a frame not for this node is
forwarded with `hops - 1` (dropped when that reaches 0): a broadcast goes
out on every link but the one it arrived on; a unicast goes out on the link
its destination was last heard on, unless that is the arrival link (split
horizon) or the destination is unknown (dropped). The duplicate drop by
`(src, seq)` is what keeps a triangle of relays from looping.

`setRelayFilter(fn, context)` installs one outbound predicate,
`bool fn(context, linkIndex, const FrameHeader&, payload, size)`, consulted
for every relayed frame on every link it would leave on, never for the
node's own sends; a refusal is counted in `filtered()`, not `dropped()`.
The ESP32 relay is the one user: on its UART link it lets unicasts through
(routed there, they are for the board) and, of the broadcasts, only those
whose envelope is an `Announce` (`envelopeIsAnnounce()` in
`protocol/envelope.hpp`: one byte compared, the body's tag), so the LAN's
telemetry never costs the 921600 baud line anything. The hub learns the
board from the board's own Announce, relayed, at the relay's IP and data
port, and its unicasts to the board land there and are relayed down the
UART; the relay's own Announce travels beside it, from its own node id.
`relayed()` counts the frames forwarded, one per link for a broadcast.

## Links

- `AbsLink` (`transport/link.hpp`): `send(frame, size, address)`,
  `broadcast(frame, size)`, `receive(buffer, capacity, addressOut)`, all
  non-blocking. `LinkAddress` is opaque to everyone but the link: an IPv4
  host and port for UDP, zeros for a UART.
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
  deployment must agree on; the constructor takes another one so a batch
  campaign isolates itself from a live bench. `discoveryFd()` /
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
counters, the same beacon and expiry rules, the `(src, seq)` duplicate
drop, no relay. Its two sockets follow the `UdpLink` layout, with one
substitution forced by the engine: Godot's `PacketPeerUDP.bind()` sets no
reuse option, so the discovery socket is a `UDPServer` (`listen()` sets
`SO_REUSEADDR`, which is enough on Linux to share the port with the
`SO_REUSEADDR + SO_REUSEPORT` sockets of the C++ nodes). The plant hosts
one virtual drone per `DRONE_SIM` node it hears, and the lockstep
exchange (`SimSensor`, `SimActuator`, `SimScenario`) is unicast frames
between the plant's node id and each `drone_sim`'s: `drone_sim` adopts as
its plant the first node whose `SimSensor` validates, until the transport
forgets it (`platform_sim/plant_link.hpp`). Nothing is configured, no
port is reserved. `sim-godot/tests/transport_check.gd` is its ctest
smoke, `test_plant_link.cpp` exchanges frames with it from C++.

## Ports

| port | who | what |
|------|-----|------|
| udp/47820 | every transport node | discovery: broadcast frames (beacons, telemetry, answers) |
| ephemeral | every transport node | data socket: unicast frames (commands, lockstep sim link, beacon on first sight) |
| udp/47810 | hub | HTTP + WebSocket for the pages |

The ESP32 relay owns no port of its own: it is one more node on udp/47820
with an ephemeral data socket, and the board's UART carries transport
frames in the serial framing. It shares the address the board is seen at:
two node ids, one IP and one data port.

`drone_sim` and the firmware send telemetry, log lines and every answer
(tuning, OTA, run stats) as broadcast frames, so the hub, a batch campaign
and any other node read the same stream; commands reach them as unicasts to
their node. Their beacon is the `Announce` envelope (kind, name, mcu, build
identity, wire hash). The board's node id is `hashNodeId()` of the 96-bit
MCU unique id (`boardNodeId()`), so it survives resets and reflashes; the
hub knows a board is a board from the `kind` of its Announce, not from the
link it arrived on.

## Open points

- No retransmission, no acknowledgement, no fragmentation: what the
  application needs it does itself (the OTA client already does).
- Unicast replies are not used by `drone_sim` nor by the firmware: every
  answer is a broadcast, which is the simpler option and what the ground
  tools expect today (the ESP32 relays the board's onto the LAN).
- The relay filter is one predicate and one rule: no routing table, no
  per-node policy. A second kind of node behind a UART would share the
  same rule.
- The board's transport keeps the full `MAX_NODES` = 32 table (about
  1.3 KB) although it only ever sees a handful of nodes; RAM is not tight
  on the F405 so nothing shrinks it.
