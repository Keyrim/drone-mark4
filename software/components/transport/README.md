# transport

The interface manager between the processes and boards of the project. An
application declares its physical links (one per medium: a UDP socket pair,
a UART), then calls `send(node, payload)`; the transport remembers on which
link and at which address every node was last heard and emits there. The
payload is opaque: today it is a `protocol/` packet, and the transport does
not link `protocol/` (it depends on `drone_warnings` alone). The core and
the UART link build for every preset, the F405 included, with no heap and
fixed-size tables; the UDP link is POSIX and desktop only.

Adopted today between `drone_sim` and the `hub`, and by the batch campaign
(`tools/batch/run_batch.py`, through the frame codec of
`tools/telemetry_wire.py`). The firmware, the bootloader, the ESP32 bridge
and the hub's bridge/serial path still speak bare `protocol/` packets.

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
the serial framing (`transport/serial_framing.hpp`: `A5 5A len payload
crc16`, CRC-16/CCITT-FALSE over length and payload), whose one length byte
caps a UART frame at 255 bytes, header included.

## Node ids

Transport-level `uint32_t`, self-assigned at start, never configured, never
0. A desktop process draws one from `/dev/urandom` (`randomNodeId()`);
`drone_sim --node-id N` pins one so a batch campaign knows which node is
which run. An embedded target folds its MCU UID or MAC through
`hashNodeId(bytes, size)` (FNV-1a); nothing embedded uses it yet.

## API (`transport/transport.hpp`)

```cpp
Transport transport(nodeId);           // explicit, value member of the App
transport.addLink(udpLink);            // up to MAX_LINKS = 4, AbsLink&
transport.init();                      // false without a node id or a link
transport.setBeacon(bytes, size);      // optional, at most MAX_BEACON_SIZE = 64
transport.setRelay(true);              // off by default
transport.setNodeCallbacks(onUp, onDown, context); // function pointer + context
transport.send(dst, payload, size);    // dst 0 = broadcast on every link
transport.poll(nowUs, deliver, context); // drain, learn, deliver, relay, expire, beacon
transport.isAlive(id); transport.findNode(id); transport.nodeCount(); transport.node(i);
```

`poll()` is the only place anything happens, and `nowUs` comes from the
caller: the transport never reads a clock. Every frame received, whatever
its payload, refreshes the node table (`nodeId -> link, address,
lastSeenUs, lastSeq, received, lost, duplicates`, `MAX_NODES` = 32); a
payload addressed to this node or to everyone is handed to `deliver`. A
frame whose `src` is this node (its own broadcast coming back on a shared
medium) is ignored.

Sequence accounting per node: an exact repeat of the last sequence is a
duplicate and is dropped; a forward gap below `RESYNC_THRESHOLD` (1024) is
counted as lost frames; a larger jump is a restarted sender and counts as
nothing. The hub's `status.links[]` carries these counters as `transport`
entries.

## Presence

`setBeacon()` registers an application payload (the `AnnouncePacket` for a
flight process) that the transport broadcasts every `BEACON_PERIOD_US`
(1 s, the first one on the first `poll()`), and additionally unicasts once
to a node the moment it first appears, so a newcomer learns everyone at
once. A node silent for `NODE_EXPIRY_US` (3 s) is forgotten and `onDown`
fires; `onUp` fires when a node is heard for the first time. The hub sets
no beacon: a flight process learns it from the first command it sends.

## Relay

Off by default. With `setRelay(true)`, a frame not for this node is
forwarded with `hops - 1` (dropped when that reaches 0): a broadcast goes
out on every link but the one it arrived on; a unicast goes out on the link
its destination was last heard on, unless that is the arrival link (split
horizon) or the destination is unknown (dropped). The duplicate drop by
`(src, seq)` is what keeps a triangle of relays from looping. Built for the
ESP32 bridge (UART on one side, WiFi on the other); nothing relays yet.

## Links

- `AbsLink` (`transport/link.hpp`): `send(frame, size, address)`,
  `broadcast(frame, size)`, `receive(buffer, capacity, addressOut)`, all
  non-blocking. `LinkAddress` is opaque to everyone but the link: an IPv4
  host and port for UDP, zeros for a UART.
- `UdpLink` (`transport/udp_link.hpp`, POSIX): one shared **discovery
  port** every node binds with `SO_REUSEADDR + SO_REUSEPORT` and only ever
  receives broadcasts on, and one ephemeral **data socket** every frame
  leaves from, broadcasts included, so the source port of any datagram is
  the node's unicast address. Broadcast = `sendto 255.255.255.255:discovery`;
  when the host has no route for it (an isolated container) the link falls
  back to `127.255.255.255`, the loopback broadcast, which Linux also
  delivers to every local listener. `DISCOVERY_PORT` = 47820 is the one
  port a deployment must agree on; the constructor takes another one so a
  batch campaign isolates itself from a live bench. `discoveryFd()` /
  `dataFd()` let a caller `poll(2)` instead of spinning.
- `UartLink` (`transport/uart_link.hpp`): owns no hardware, is handed an
  `AbsByteStream` (`read`, `write`) and applies the serial framing;
  resynchronizes on the sync pair after garbage or a torn frame (a torn
  frame swallows the next one up to its announced length, then the CRC
  fails and hunting resumes).

## Ports

| port | who | what |
|------|-----|------|
| udp/47820 | every transport node | discovery: broadcast frames (beacons, telemetry, answers) |
| ephemeral | every transport node | data socket: unicast frames (commands, beacon on first sight) |
| udp/47800 | drone_sim <-> Godot | lockstep sim link, bare packets (`protocol/ports.hpp`) |
| udp/47802 | Godot -> hub | sim raw broadcast, bare packets |
| udp/47810 | hub | HTTP + WebSocket for the pages |
| udp/47830, 47831 | ESP32 bridge | pseudo-serial stream and bridge announce |

`drone_sim` sends telemetry and every answer (tuning, OTA, run stats) as
broadcast frames, so the hub, a batch campaign and any other node read the
same stream; commands reach it as unicasts to its node. Its beacon is the
`AnnouncePacket` with `sessionId` = node id and both port fields at 0
(meaningless since the transport; the packet dies with the protobuf layer).

## Open points

- UART frames are capped at 255 bytes by the one-byte serial length, so an
  `OtaChunkPacket` (251 bytes) does not fit behind the 11-byte header. The
  board migrates later; the chunk size or the length field will move then.
- No retransmission, no acknowledgement, no fragmentation: what the
  application needs it does itself (the OTA client already does).
- Unicast replies are not used by `drone_sim`: every answer is a broadcast,
  which is the simpler option and what the ground tools expect today.
