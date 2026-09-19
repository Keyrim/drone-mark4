# protocol

The wire of the project: two schemas and the codecs the build generates
from them. `mark4.proto` is THE wire: every datagram and every serial frame
between the flight processes, the board, the plant and the ground tools
carries exactly one `Envelope`, a `oneof` over every message of the system
(the status report and its subscription, the telemetry family, the lockstep
sensor and actuator frames, RC, the identity pair, the log line, the module
table and the level move, scenario, tuning, updater, and the `RequestAck`
of the messenger). `gateway.proto` (imports it) is
the contract between the hub and its websocket clients: `GatewayMessage`,
a `oneof` over the `NodeTable`, one message per node and per concept (its
log modules and its lines, its last `Status`, its telemetry descriptors,
configuration and samples, its tuning table and results), the commands a
client sends (telemetry, log, tuning, pilot input, node, plus the
`OtaCommand` of the updater and the `ProfileCommand` of the tuning
profiles), the `GatewayStatus`, the `OtaState`, the `ProfileList` /
`Profile`, and `Ack`. No encoded `Envelope` crosses it in either direction:
the hub decodes everything it hears and publishes typed messages, and a
client sends typed commands. It never crosses the LAN. Its bodies share one
nanopb struct, so anything per-node and unbounded gets a message of its own
rather than a field in `Node`. Nothing generated is committed.

| Consumer | Generator | Output | When |
|----------|-----------|--------|------|
| every C/C++ target, desktop and STM32 | nanopb (FetchContent, `nanopb-0.4.9.2`, `PB_NO_MALLOC`, `PB_BUFFER_ONLY`, `PB_NO_ERRMSG` on the board) | `software/build/<preset>/gen/nanopb/mark4.pb.{c,h}` | the `nanopb` target, on every build |
| the hub (gateway.proto, desktop only) | nanopb, `gateway.options` | `software/build/desktop/gen/nanopb/gateway.pb.{c,h}` | the `nanopb_gateway` target of the desktop preset |
| the web pages (both schemas) | `protoc-gen-es` (`@bufbuild/protoc-gen-es`, npm) run by the `protoc` of `grpcio-tools` | `software/hub/pages/src/gen/{mark4,gateway}_pb.ts` (gitignored) | `pnpm gen`, run by every pnpm script of `software/hub/pages` |
| the Godot plant | godobuf, the addon committed in `sim-godot/addons/godobuf/` (pinned commit, BSD-3) run by a headless Godot through `scripts/gen_godobuf.py` | `sim-godot/scripts/gen/mark4.gd` and `wire_hash.gd` (gitignored) | target `proto_gd` of the desktop preset, when `godot` is on the PATH |

`mark4.options` bounds every field for nanopb (string sizes, fixed-count
float vectors, the 240-byte chunk, two OTA slots) so the structs hold their
storage inline, and gives every enum a fixed 32-bit underlying type so an
unknown value from a newer peer is a plain integer to compare, never an
out-of-range enum load. The generator needs the python packages `protobuf`
and `grpcio-tools` (the devcontainer image ships them); the configure step
says so when they are missing.

## What the C++ side gets

- `protocol/envelope.hpp`: `mark4.pb.h` plus `encodeEnvelope()` /
  `decodeEnvelope()`, buffer in, buffer out, no allocation, and
  `MAX_ENVELOPE_SIZE` (437 bytes today: a full `TelemetryData` batch of 32
  values, ahead of the 390-byte telemetry configuration and the 255-byte
  OTA chunk). Static asserts
  keep `sizeof(mark4_Envelope)` under 400 bytes, and the transport and
  serial framing check that every envelope fits their payloads. It is the
  one header the schema enters C++ through: it re-exports `mark4.pb.h`,
  `pb.h` and `protocol/wire_hash.hpp` (IWYU pragmas, honoured by clangd),
  so a file that uses a `mark4_*` type or `WIRE_HASH` includes it and
  never the generated headers directly.
- `protocol/wire_hash.hpp`: `WIRE_HASH`, the first 8 hex characters of the
  SHA-256 of `mark4.proto`, computed by CMake at configure time and
  regenerated on every edit of the schema. Every `Announce` carries it; the
  hub publishes its own in `GatewayStatus.wire_hash` and every node's last
  `Announce` in the `NodeTable`, the console shell compares the two and
  paints a mismatching chip red. `gateway.proto` is not part of the hash:
  the pages are generated from the same tree as the hub. The packaging
  script stamps it into the `.ota` manifest (`wireHash`) and the hub
  refuses a bundle built on another schema. The Godot plant reads its own
  from `wire_hash.gd`.
- `protocol/ota_image.hpp`: what is not wire but still crosses processes:
  the on-flash `OtaImageHeader`, the slot and chip identities in their
  flash encoding (`OTA_SLOT_*`, `OTA_MCU_*`, EMPTY is 0xFF on flash and 0
  on the wire, `otaSlotStateToWire()` maps), the chunk size and window.
Enum values are C-scoped inside the package, so two enums never share a
value name: `PHASE_*`, `THROW_*`, `RC_*`, `OTA_OK`, `OTA_OP_*` carry the
prefix the clash forced, the rest stay short. The flight core's own enums
(FlightPhase, ThrowState, PilotMode, TuningStatus) are pinned to the wire
value by value in `status/status_packer.hpp` and `tuning/provider.hpp`, and
`TelemetryUnit` is pinned to the leaf library's own enum in
`telemetry/provider.hpp`; flight-core never includes this library.

## Requests and answers

Every message is a unicast: nothing but the transport's own keepalive is
broadcast. A message that has to arrive carries a non-zero `request_id`
(field 100 of the `Envelope`, outside the oneof) and the receiving
messenger answers a `RequestAck` with that id on arrival, before it is
dispatched and whether or not any handler claims the body. The sender
resends until it comes back, then gives up
(`software/components/messaging/README.md`). An acknowledgement says
"arrived" and nothing about what was done, so an answer that matters is a
request of its own, correlated by its content and never by the id it
answers. Stream data (`Status`, `TelemetryData`, `Log`, `Rc`) carries no
id: a sample is only worth its own instant, and the subscription is what
guarantees the stream as a whole.

A request and its answer are two bodies, never one, because a node that is
the provider of a concept and a consumer of the same concept (the hub, for
its own log lines) holds one handler per body tag: the provider claims the
request tags and the consumer the state tags, and the two never meet.

## Where the messages travel

- `Status`, `TuningAck`, `TuningInfos`, the `Ota*` answers: flight process
  to ground, as unicasts to the node that asked or subscribed (drone_sim,
  and the board through the ESP32 relay). `Status`
  is the small fixed report of what the drone is doing, decimated to 50 Hz
  and emitted to whoever holds a `StatusSubscribe`: attitude, motors,
  phase, throw state and count, the two
  validity flags, whether the RC uplink is heard (`rc_link_ok`, the pilot's
  device reads it as "the drone hears me"), and the plant's exact state in
  `Status.truth` when the sender has one. `Status.imu_valid` / `baro_valid` repeat the validity
  flags of the frame that was stepped (a fresh measurement acquired for
  that frame, see `software/components/platform/README.md`);
  `PHASE_FAULT` is the flight core's latched motors-off state after the IMU
  was lost with the motors running.
- `IdentityRequest` and `Announce`, the identity pair: the request is
  unicast to one node, empty, and the node's `Announce` (kind, name, mcu,
  build identity, wire hash) is the unicast answer back to the requester,
  a request of its own so the asker acknowledges it.
  `drone_sim`, the firmware, the relay, the hub, the plant and the phone
  answer (`software/components/discovery/`); the hub, the plant and the
  phone ask. Presence on the wire
  stays the transport's keepalive, which carries the sender's boot id and
  no identity: nothing sends an `Announce` unsolicited.
- The telemetry family, all unicast:
  `TelemetryListRequest { cursor }` (ground to node) is answered by one
  `TelemetryDescriptors` page back to the requester;
  `TelemetrySubscribe { enabled }` takes the sample stream or stops it and
  is answered by a `TelemetrySubscription`; `TelemetryConfigure { ids,
  period_ms }` replaces the enabled set and the period wholesale, one
  configuration per node and never one per consumer, and is answered by
  the `TelemetryConfig` in effect, sent to the node that asked and to every
  other subscriber; `TelemetryData` carries one sampling instant of
  up to 32 values, split into several messages of the same timestamp when
  more are enabled. A measure is named by a stable path and routed by the
  id of the node's frozen table: see
  `software/components/telemetry/README.md`.
- The log family, all unicast: `Log` (one line, module by id) goes to the
  nodes that sent a `LogSubscribe { enabled }`, answered by a
  `LogSubscription`; `LogModulesRequest { cursor }` is answered by one
  `LogModules` page of the node's module table; `LogSetLevel { module_id,
  level }` moves one threshold and is answered by that module's
  `LogModuleInfo`, sent to the requester and to every other subscriber of
  the line stream. Every node speaks them, the hub included (its own lines
  leave from its node id). See `software/components/log/README.md`.
- `Rc`, `Reboot`, `SimScenario`, `TuningSet`, `TuningGet`,
  `TuningListRequest`, the `Ota*`
  requests: ground to flight process, as transport unicasts or serial
  frames. `Rc` is the pilot state as a stream (kill, arm, mode, throttle
  and the three sticks in the pilot's convention: positive is right,
  forward, clockwise), repeated at 20 Hz by a web page and 50 Hz by a
  gamepad path; the flight process treats 200 ms of silence as a kill
  (`platform_common/rc_tracker.hpp`) and reports whether it hears a pilot
  in `Status.rc_link_ok`. The sticks are raw positions: deadband, ranges
  and the mapping onto the body frame are the flight core's, tunable like
  a gain (`stick_*` in the tuning table).
- The transport family, all unicast: `TransportSubscribe { enabled }`
  takes a node's report stream or stops it and is answered by a
  `TransportSubscription`; `TransportReport` is one node's view of the
  wire, sent once a second to whoever subscribed. It carries that node's
  transport and messenger counters, one `TransportLink` per declared link
  (the medium as a `LinkKind`, the frames and bytes each way, what the
  medium refused and what it could not deliver whole) and its peer table
  as `TransportPeer` entries, by pages of four named by `peer_cursor` and
  `peer_total`. Every number is cumulative, so a lost report skews nothing
  and the reader takes the differences. Every C++ node answers
  (`software/components/transport/README.md`).
- `SimSensor` (truth included) and `SimActuator`: the lockstep exchange
  between a flight process and its plant, transport unicasts between the
  two node ids; `SimScenario` is forwarded to the plant the same way as
  its own envelope, once per scenario, and the plant plays it once per
  change of `sequence`.

The serial framing (`transport/serial_framing.hpp`) carries one envelope
per frame behind a two-byte length, 512 bytes at most like the transport's
`MAX_PAYLOAD`. The transport's keepalive is not an envelope at all: 11
bytes of header, flagged in the `flags:hops` byte, plus a 4-byte boot id
(`software/components/transport/README.md`).

## Changing the schema

A field number a message stops using is named in a comment and never
reused; `reserved` is not written, because the godobuf generator of the
Godot plant parses no such statement and would fail on the schema.

Edit `mark4.proto` (and `mark4.options` when a bound moves), rebuild: the
codecs regenerate, the wire hash changes, and every node built before
shows up as a wire mismatch on the pages instead of a silent bench. A board
running the previous schema still has to be reflashed once over SWD (or
updated by a hub built on the previous schema): see `docs/ota-design.md`.
The C++ unit tests round-trip every message
(`software/tests/unit/test_protocol_envelope.cpp`) and one test spawns a
headless Godot to exchange envelopes with the generated GDScript codec
through the two transports (`test_plant_link.cpp`, skipped without a godot
binary).
