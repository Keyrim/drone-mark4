# Communication stack - target design

Status: implemented on branch `refactor/comm-stack-v2` (issue #29), one
pull request pending. Section 2 describes the code as it was on `main`
before the rework; sections 3 to 6 describe what the branch built; section
8 is the decision that shapes the next rework (the gateway API, the Dart
and GDScript ports as the only ports); section 9 lists what was left out.
Companions: `docs/target-architecture.md` (the system this plugs into,
sections 3.3 to 3.5 describe the present protocol and hub),
`software/components/transport/README.md` (the transport as it is),
`docs/ota-design.md` (the one application protocol with its own state
machine, unchanged by this document).

## 1. Goal and scope

Split what today is one blurred layer into three components with one job
each:

- **transport**: presence. Who is reachable, on which link, at what
  distance, and moving frames of opaque bytes between node ids.
- **messaging**: the postman and the sender. One place where an `Envelope`
  meets the transport, in both directions: decode and dispatch by message
  type to registered handlers, encode and send to a node id.
- **discovery**: identity. Who a node id is (kind, name, chip, build, wire
  hash), obtained on request by the nodes that care.

In scope: the C++ components under `software/components/`, the six sites
that compose them today (hub, drone_sim, drone_firmware, the ESP32 relay,
the mobile shim, the Godot plant), the Python node of the batch tool, and
the wire changes this implies. Out of scope, listed with reasons in
section 9: threading the transport, a port layer for the clock, and the
subscription model for status, logs and telemetry.

## 2. What the present code does, and why it is being reworked

The findings of a code reading of `transport/` and of every node that uses
it. Each one alone is a small thing; together they show a transport that
knows the wire it is supposed to ignore, and applications that re-implement
the same plumbing by hand.

### 2.1 The keepalive is an application message

`Transport::setBeacon()` stores an opaque blob the transport broadcasts
every second and unicasts to every newcomer. The blob is always the
`Announce` envelope. So `MAX_BEACON_SIZE` (64) is sized for a protobuf
message (45 bytes worst case) the transport claims to know nothing about;
the constant is duplicated in `transport_shim.h` with a `static_assert`,
and the hub, drone_sim, the firmware, the relay, the shim and the Godot
port each build the same envelope and each check the fit their own way.

### 2.2 Identity is pushed to everyone, and the drone never reads it

drone_sim and the firmware only ever build an `Announce`; they never decode
one they receive. The plant is adopted on the first `SimSensor` that
validates, from the `src` the transport reports, not from any identity.
Only the ground side (hub, pages, Godot, phone) keeps a table of who is
who. Yet every node broadcasts its identity at 1 Hz to every other node,
and the relay carries every LAN node's identity down the UART so the board
can keep them alive in a table it never consults for anything but
addressing.

### 2.3 `linkMask` and `setRelayFilter()` patch the same problem twice

The ESP32 relay has two links (UART to the board, UDP to the LAN). Because
its own log lines are broadcast, `send()` grew a `linkMask` so the relay
can keep them off the UART; the link indexes it relies on (UART = 0,
LAN = 1) are hand-written and depend on the `addLink()` order. Because the
LAN's broadcasts would otherwise flood the UART, `setRelayFilter()` lets
the relay peek at the envelope tag of every forwarded broadcast and pass
only `Announce`. Both exist because broadcast is the default way of
speaking; neither would be needed if it were not.

### 2.4 There is no postman

`DeliverFn` is an argument of `poll()`, not a subscription: one raw-bytes
receiver per call. What happens next differs per node:

- drone_sim and the firmware: a trampoline pushes the bytes into
  `CommandReceiverTransport` (a ring of 32 slots of 512 bytes), the App
  drains it once per flight frame, decodes, then runs a hand-written chain
  of responsibility (the OTA updater looks first, then the telemetry
  service, then a `switch` on `which_body`, `default` to tuning). The chain
  is App code, duplicated between `drone_sim_app.cpp` and
  `firmware_app.cpp` with small divergences.
- the hub: decodes inline in the callback, after mirroring the raw bytes to
  every websocket client.
- the relay: peeks the tag without decoding (`envelopeBodyTag()`), decodes
  only if `answeredHere(tag)`. The one place that treats the tag as a
  dispatch key.

Adding a service means wiring it by hand in every App, in the right place
of the chain. Nothing in the project registers itself with a postman the
way a `TelemetryEntry` or a `LogModule` registers itself with its registry
at construction.

### 2.5 Two ways to send an envelope

`platform_common/envelope_io.hpp` encodes on the stack and calls
`transport.send()`; it knows the transport. `log_wire` does not: it takes a
`LogSendFn` function pointer and the App connects it. The wire services
(`telemetry_service`, `tuning_service`, `status_publisher`, `envelope_io`)
live in `platform_common` although nothing about them is platform; they
are there because there was no other floor.

### 2.6 The UDP address shape leaks into the public header

`LinkAddress` is `{uint32 ipv4 host, uint16 port}`, in
`transport/link.hpp`. The transport copies it in `learn()` and hands it
back to the same link in `send()` and `relay()`; it never reads a field.
The UART leaves both at zero.

### 2.7 Smaller points

- One `onNodeUp` and one `onNodeDown` callback, while discovery, the
  telemetry pull and the hub's table all need to know.
- `hops` counts down from 4 and the frame dies at 1, so the receiver never
  knows how far a frame travelled.
- The transport never reads a clock: `poll(nowUs)` propagates the caller's
  instant to `learn()`, `expire()` and the beacon. This is the right rule
  (flight-core and log follow it) and stays; the alternative, a port layer
  the transport would depend on, is out of scope.

## 3. Transport v2

The transport's job is presence: a reliable up/down for every node in
reach, and moving opaque bytes between node ids. It knows no message.

### 3.1 Wire

The frame header stays `src u32, dst u32, seq u16, hops u8`, little-endian,
11 bytes, followed by an opaque payload of at most 512 bytes. Two changes:

- **The keepalive is an empty frame.** A header alone, no payload, `dst =
  BROADCAST_NODE`, once per `KEEPALIVE_PERIOD_US` (1 s), plus one unicast to a
  node the moment it first appears so both tables converge within one poll.
  The receiver learns the sender from it as from any frame and delivers
  nothing upward: a payload of size 0 is consumed by the transport. This is
  the only wire broadcast in the system.
- **Hops count up.** A frame leaves with `hops = 0`, every relay adds one,
  and a relay drops a frame that already carries `MAX_HOPS`. At the
  receiver `hops` is the number of relays crossed: 0 means a direct
  neighbour. `Node` gains that distance, refreshed on every frame.

Every link already carries a header-only frame: UDP as an 11-byte datagram,
the serial framing with a length of 11 (its zero-length refusal is on the
whole transport frame, never on the payload), `decodeFrameHeader()` accepts
exactly 11 bytes. The consumers that decode envelopes already return false
on 0 bytes. Only the semantic changes.

### 3.2 API

```
class Transport
{
    Transport(uint32_t nodeId);
    bool addLink(AbsLink &link);                 // index = order of calls
    bool init() const;
    // presence listeners attach themselves (AbsPresenceListener below)
    bool send(uint32_t dst, const uint8_t *payload, size_t size);
    void poll(uint64_t nowUs, DeliverFn deliver, void *context);
    const Node *findNode(uint32_t id) const;
    // counters unchanged: dropped, sent, sentBytes, refused, relayed
};
```

Gone: `setBeacon()`, `MAX_BEACON_SIZE`, the `linkMask` parameter of
`send()`, `setRelay()`, `setRelayFilter()`, `filtered()`,
`setNodeCallbacks()`.

```
class AbsPresenceListener
{
  public:
    explicit AbsPresenceListener(Transport &t);   // t.attach(*this)
    virtual ~AbsPresenceListener();                // t.detach(*this)
    virtual void onNodeUp(const Transport::Node &node) = 0;
    virtual void onNodeDown(const Transport::Node &node) = 0;
};
```

A listener registers itself: its constructor takes the transport and
attaches, its destructor detaches, the way a `TelemetryEntry` links into
the telemetry registry and a `LogModule` into the log registry. The
transport holds up to `MAX_LISTENERS` references in a fixed table (no
heap), calls them in attach order from inside `poll()`, and a table that
is full is an init failure, not a silent drop. Calling `send()` from a
listener is allowed: the transport already does it for the unicast
keepalive (TX and RX buffers are distinct).

`DeliverFn` stays an argument of `poll()`: the transport is passive, every
byte moves during a `poll()` the application drives, and the one caller of
`poll()` in the target design is the messaging component (section 4).

### 3.3 Addresses

```
struct UartAddress {};                          // point to point
struct UdpAddress { uint32_t host; uint16_t port; };
using LinkAddress = std::variant<UartAddress, UdpAddress>;
```

Defined in `transport/link.hpp` next to `AbsLink`. The transport stores a
`LinkAddress` per node and hands it back to the link the node was heard on;
it never visits it. A link reads its own type with `std::get_if` (never
`std::get`, which aborts under `-fno-exceptions`) and refuses the other.
Adding a medium is one type and one alternative in the alias.

Why not a class hierarchy `Node -> NodeUart / NodeUdp`: the table is a
fixed array of 32 `Node` by value, with no heap and no RTTI. Polymorphic
objects of different sizes need a pointer and storage owned elsewhere,
which is a second table with its own lifecycle to keep in step with the
first. The variant is the by-value form of the same idea on a closed set of
types, which is what a list of media is.

### 3.4 Relay

Every node relays; there is no switch. The rule is the present one: a
broadcast (only the empty keepalive, now) goes out on every link but the
one it arrived on; a unicast goes out on the link its destination was last
heard on, unless that is the arrival link (split horizon) or the
destination is unknown. On a node with a single link both branches are
empty, so the hub, the drones, the phone and the plant relay nothing
without having to say so; the ESP32 is the one node where the rule does
work. The duplicate drop by `(src, seq)` still breaks loops. The filter is
gone: with no application broadcast left, an 11-byte keepalive per LAN
node per second is what the UART carries, instead of a 45-byte `Announce`
per LAN node per second.

### 3.5 Presence and the table

Unchanged: `MAX_NODES` = 32, `NODE_EXPIRY_US` = 3 s (three missed
keepalives), `RESYNC_THRESHOLD` = 1024, the per-node counters (received,
lost, duplicates). `learn()` keeps overwriting link and address on every
frame, so a node moves to the last link it was heard on.

## 4. Messaging

A new component, `software/components/messaging/`, static library, links
`transport` and `protocol`, builds for the F405, the ESP32 and the desktop
(no heap, no iostream, no exceptions). It is the one place an `Envelope`
meets the transport.

### 4.1 Receiving: the postman

```
class AbsMessageHandler
{
  public:
    // tags: the which_body values it consumes, a span over a static constexpr
    // array of the derived class. It travels through the constructor because
    // a virtual call there would run before the derived vtable exists.
    explicit AbsMessageHandler(Messenger &m, std::span<const pb_size_t> tags);  // m.attach(*this)
    virtual ~AbsMessageHandler();                                               // m.detach(*this)
    std::span<const pb_size_t> tags() const;
    virtual bool onMessage(uint32_t src, const mark4_Envelope &e, uint64_t nowUs) = 0;
};

class Messenger
{
  public:
    explicit Messenger(Transport &transport);
    void setTap(TapFn tap, void *context);   // optional: raw bytes of every delivered payload
    void poll(uint64_t nowUs);               // polls the transport, decodes, dispatches
    bool send(uint32_t dst, const mark4_Envelope &e);   // encode + transport.send
    // attach / detach are called by AbsMessageHandler alone
};
```

One handler per message type. A handler hands the `which_body` values it
consumes to the base constructor, which attaches it; its destructor
detaches. A claim is made once, at construction: a handler whose tag was
already taken owns no slot, hears nothing on it, never takes it over, and
`init()` reports it for as long as it lives. Attaching fills a table indexed by tag
(one slot per `Envelope` body, about forty), so dispatch is a direct
lookup and there is no chain and no order: a tag that is already taken is
an init failure, reported by the App's `init()` like any other. Declaring
the handler as a member of the App is the whole wiring:

```
OtaHandler m_ota{m_messenger, m_updater};
TelemetryHandler m_telemetry{m_messenger, m_telemetryService};
```

`poll()` is the one caller of `Transport::poll()`. For each delivered
payload: the tap, if set, sees the raw bytes with their `src` (this is the
hub's mirror towards its websocket clients, which forwards frames without
interpreting them); then the `Envelope` is decoded (a payload that does not
decode is counted and dropped) and the handler of `which_body` is called.
A tag without a handler is counted and dropped. The return value of
`onMessage()` says whether the handler acted on the message; it feeds a
counter, nothing else.

The ring of `CommandReceiverTransport` disappears from the drone Apps: the
`Messenger` is polled once per flight frame, at the point where the ring
used to be drained, so the pacing is the same and there is no buffering
between the transport and the handlers. If a later step threads the
transport, the ring comes back inside the messenger as its RX queue, which
is where it belongs.

The relay's `answeredHere(tag)` becomes the `tags()` of its handlers; the
peek without decode is no longer needed since nothing is forwarded through
the messenger (forwarding is the transport's relay, below it).

### 4.2 Sending

`Messenger::send()` replaces `platform_common/envelope_io.hpp`'s
`sendEnvelope()`: encode on the stack into `MAX_ENVELOPE_SIZE`, hand the
bytes to `Transport::send()`, best effort, no retry. A service that wants
to send holds a `Messenger&` and names its destination; nothing else in the
project encodes an envelope for the wire.

`log_wire` keeps its `LogSendFn` injection (the log library must not depend
on messaging, it is a leaf); the App binds that function pointer to
`Messenger::send()` towards the log subscribers (section 9.4 for what
"subscribers" means until that step lands: the hub's node id, and only when
one is known).

### 4.3 Destinations

`BROADCAST_NODE` is not a valid destination for `Messenger::send()`; it is
refused and counted. An application that wants to reach "everyone it
knows" iterates the nodes it is interested in and unicasts to each. Who
those nodes are comes from discovery (section 5).

### 4.4 What moves out of `platform_common`

`telemetry_service.hpp` and `tuning_service.hpp` are wire services, not
platform: they move to a `services/` component (header-only, linking
messaging, flight_core, telemetry, log and ota) as handlers of the
messenger, joined by an `OtaService` that replaces the two identical
`serveOta()` of the Apps. `command_receiver_transport.hpp` and the
`AbsCommandReceiver` interface are deleted: commands are not a platform
service. `PlantLink` (platform_sim) is the handler of `sim_sensor` and the
caller of `Messenger::poll()` in the sim, from the sensor wait.

Two time bases meet in the sim: the messenger is polled on the process
clock from the sensor wait, the flight frames carry the plant's time. A
handler that times something against the frames must not use the poll's
instant: the telemetry service stamps an enable with the timestamp of its
last `sample()`, the sim's RC handler stamps a packet with the last frame
received (one period behind at most, invisible to the fail-safe timeout).

What still broadcasts stays where it is until it has a destination
(section 5 for the directory, 9.4 for the subscriptions): `envelope_io.hpp`,
`status_publisher.hpp` and the sim run tracker keep `sendEnvelope(transport,
BROADCAST_NODE, ...)`, the log sink keeps its raw broadcast. What stays in
`platform_common` for good is what is about the platform (`RcTracker`,
`packStatus`, `FrameTelemetry`).

## 5. Discovery

A new component, `software/components/discovery/`, static library, links
`messaging`. Identity is pulled by whoever needs it, never pushed.

### 5.1 Wire

Two messages in `mark4.proto`:

- `IdentityRequest {}`: empty, unicast to the node whose identity is
  wanted.
- `Announce`: unchanged fields (kind, name, mcu, build_epoch, git_hash,
  wire_hash), now only ever sent as the unicast answer to an
  `IdentityRequest`. It is never broadcast and never sent unsolicited.

### 5.2 Two levels

```
class Discovery : public AbsMessageHandler
{
  public:
    Discovery(Messenger &m, const mark4_Announce &self);
    // tags(): identity_request; answers with self to src
};

class DiscoveryDirectory : public Discovery, public AbsPresenceListener
{
  public:
    DiscoveryDirectory(Messenger &m, Transport &t, const mark4_Announce &self);
    // tags(): identity_request, announce
    // on node up: a PENDING entry; tick() asks, retries on timeout, gives up
    // on announce: stores per node id, tells the listeners; on node down: forgets
    void tick(uint64_t nowUs);      // the directory reads no clock: the composition's loop calls it
    size_t nodesOfKind(std::span<const mark4_NodeKind> kinds, std::span<DirectoryEntry> out) const;
    const DirectoryEntry *find(uint32_t id) const;
    // DirectoryEntry = node id, state PENDING | KNOWN | MUTE, the Announce,
    // wireMismatch, distance in hops, instants of the last request and change
};

class AbsDirectoryListener   // self-registering, up to 4 per directory
{
    virtual void onIdentity(const DirectoryEntry &entry) = 0;   // KNOWN, or the announce changed
    virtual void onForgotten(uint32_t nodeId) = 0;
};
```

`Discovery` is mandatory on every node: a node that cannot say who it is
does not exist for the ground tools. `DiscoveryDirectory` is for the nodes
that need to know who is around: the hub, the phone, the Godot plant (one
virtual drone per `DRONE_SIM` node), the batch tool. The drones carry
`Discovery` alone.

The names say the relation: a `DiscoveryDirectory` is a `Discovery` that
also keeps a directory. Public inheritance: a service that takes a
`Discovery&` in its constructor can be handed a `DiscoveryDirectory`
without change, which is how the Apps inject dependencies today. The
derived class extends `tags()` with `announce` and overrides `onMessage()`
to handle it before deferring to the base for the request.

### 5.3 Behaviour

- On `onNodeUp` the `DiscoveryDirectory` creates a `PENDING` entry; the
  next `tick(nowUs)` sends one `IdentityRequest` to it (the presence
  callback carries no instant, and the directory reads no clock). Without
  an `Announce` within `IDENTITY_TIMEOUT_US` (500 ms) a tick sends again,
  up to `IDENTITY_RETRIES` (5), then marks the entry `MUTE` and stops
  asking. A later frame from that node does not
  trigger a new request: the node is present and mute about itself, which
  the ground tools show as such.
- An `Announce` that arrives without a request pending is stored anyway
  (a late answer).
- The table is bounded by `MAX_NODES` of the transport: one entry per
  transport node at most.
- A `wire_hash` that differs from `WIRE_HASH` is stored and flagged, as the
  hub does today; the messenger does not refuse the node, the application
  decides what it still speaks to it.

### 5.4 What it replaces

The hub's `m_announces` map and the `sameAnnounce` / `isDroneKind` logic
around it, the `NodeAnnounce` map of the mobile `TransportManager`, the
per-node dictionary of the Godot plant, and the `Announce` building code in
every App (the identity is a constant handed to the `Discovery`, built once
in `main()` or the App constructor).

## 6. Application rules

- **A node emits nothing towards a node it does not know exists.** Every
  destination of `Messenger::send()` is a node id the transport has in its
  table, or the send is refused.
- **Application "broadcast" is a loop over known nodes.** A service that
  wants to reach every node of some kinds asks the `DiscoveryDirectory` and unicasts
  to each. A node without a `DiscoveryDirectory` (the drones) cannot broadcast
  anything; it can only answer, and only towards `src`.
- **Answers go to the requester**, the `src` of the frame that carried the
  request, never to `BROADCAST_NODE`. The OTA updater, the telemetry
  service and the tuning service address their replies so; run stats and
  the status report follow once they have a subscriber.
- **The initiator of a stream is the consumer.** Status, telemetry samples
  and log lines are streams; they leave a drone because a ground node asked
  for them, towards that node, and stop when it disappears (`onNodeDown`)
  or asks them to. This rule is stated here because it is the reason the
  drone needs no `DiscoveryDirectory`; the subscription protocol that implements it
  is section 9.4, not this rework.

## 7. Impact

### 7.1 Sites to migrate

| site | today | after |
|------|-------|-------|
| `software/hub` | `setBeacon` of an `Announce`, `m_announces`, inline decode in the deliver callback, `sendEnvelope` | `Discovery` + `DiscoveryDirectory`, `Messenger` polled by the loop, handlers for descriptors / log_modules / log_control / OTA, the raw-bytes mirror to websocket clients becomes one handler that subscribes to every tag |
| `software/drone_sim` | `setBeacon`, `CommandReceiverTransport`, the hand-written chain | `Discovery`, `Messenger` polled once per flight frame, OTA / telemetry / tuning / rc / reboot / scenario / log_control as handlers |
| `software/drone_firmware` | same as drone_sim | same as drone_sim |
| `esp32-bridge/main` | `setBeacon`, `linkMask` for log lines, `setRelayFilter`, `answeredHere` | `Discovery`, `Messenger` with the tags it answers, nothing to configure for the relay |
| `software/mobile/native` + `lib/back/transport` | `mark4_transport_set_beacon`, `NodeAnnounce` map in Dart | the shim exposes the `DiscoveryDirectory` (a C ABI over `Discovery` + `DiscoveryDirectory`, ffigen as today); Dart stops building the `Announce` and reads the directory |
| `sim-godot/scripts/transport` | `set_beacon`, per-node dictionary, GDScript transport | GDScript transport v2 (empty keepalive, hops up), plus a GDScript `DiscoveryDirectory` |
| `tools/batch/run_batch.py` | reads broadcast frames off the discovery socket, no beacon | a Python node that keeps alive, answers `IdentityRequest`, and subscribes to what it wants to read (section 9.4) or reads the streams the drone sends it |

### 7.2 What breaks on the branch, and stays broken until migrated

- The bench: no `Announce` broadcast means the hub's node table is empty
  until the hub carries a `DiscoveryDirectory`; Godot hosts no virtual drone until
  its port asks for identities; the phone connects to nothing.
- The relay: without the filter and with LAN broadcasts still emitted by
  unmigrated nodes, the UART carries them. Migrating the relay after the
  desktop nodes avoids that window.
- Every stream a drone broadcasts today (status, telemetry, logs, tuning
  answers, run stats) stops reaching the ground until the consumer asks for
  it. Section 9.4 is the missing piece; until it lands, the hub (the one
  consumer today) asks by unicast with the messages that already exist
  (`TelemetryEnable`), and status and logs go to the hub's node id when a
  `DiscoveryDirectory` entry of kind `GATEWAY` is known to the drone. This is a
  temporary asymmetry, named as such in the code, that 9.4 removes.
- Unit tests: `test_transport.cpp` (beacon, filter, hops), `test_plant_link.cpp`,
  `test_ota_e2e.cpp`, the Godot `transport_check.gd` smoke, the pages'
  tests that assume an `Announce` per node.

### 7.3 Order inside the branch

1. Design document settled for sections 3 to 6. Section 8 (one design,
   several languages) is settled before step 6b, the first step that
   touches a port; section 9 does not block anything. From 6b on, no new
   tests (section 8.3).
2. transport v2 with its tests (keepalive, hops, variant address,
   listeners; filter and beacon tests removed).
3. `mark4.proto`: `IdentityRequest`; `Announce` documented as an answer
   (its comment still says "beaconed every second": a comment change moves
   `WIRE_HASH`, so it waits for this step, which moves it anyway).
4. messaging with its tests (4a); then the wire services moved and turned
   into handlers, the two flight compositions on the messenger (4b). The
   broadcasting senders wait for step 6.
5. discovery with its tests.
6. The desktop nodes: hub, drone_sim (done); then (6b) the GDScript port
   of the transport and of discovery so the bench works again. The Python
   node is not touched (section 8.2).
7. The firmware and the relay.
8. The mobile app as a full Dart node: native UDP transport, messenger,
   discovery; the NDK build, the shim and ffigen removed.
9. Documentation: `target-architecture.md` sections 3.3 to 3.5, this
   document's status line, and the pages that still describe the beacon
   and the Announce as the basis of discovery after step 2 left them
   untouched: `README.md`, `docs/architecture.md`,
   `docs/tooling-architecture.md`, `docs/bring-up.md`, `docs/mobile-app.md`,
   `tools/batch/README.md`, `sim-godot/README.md`. The component READMEs
   (transport, protocol, hub, log, esp32-bridge) are kept in step with the
   code at every step.

## 8. One design, several languages

Decided on 2026-09-10, after the inventory below.

### 8.1 Where the logic of each concept lives today

| Concept | Server | Client logic written outside C++ |
|---|---|---|
| transport | C++ | GDScript (`transport.gd`); Python (a hand-written node in `run_batch.py`); Dart reaches the C++ through the NDK shim |
| identity | C++ (hub directory, drone_sim answers) | GDScript (`announce.gd`, the plant hosts a drone per `DRONE_SIM`); Dart (`NodeAnnounce` map); the pages display the `NodeTable` without logic |
| telemetry | C++ | the hub pulls the tables in C++, but `plots/main.ts` builds `TelemetryEnable` and its keepalive itself |
| tuning | C++ | `console/tuning.ts` builds set and list and tracks the acks; profiles are already a gateway-local service |
| log | C++ | the hub aggregates the tables; the pages and the VS Code extension build `LogControl` themselves |
| OTA | C++ | none: `OtaClient` in the hub, the pages and the extension send `OtaCommand`, a gateway-local service |
| RC | C++ | Dart (`PilotManager`, 50 Hz, latches); `console/rc.ts`; Python to arm |
| sim link | C++ (`PlantLink`) | GDScript (`sim_link.gd`, `sim_codec.gd`); Python builds `SimScenario` |

OTA is the one concept whose client logic exists once, in C++, with the
interfaces sending high-level commands. It is also the one concept designed
with a document first.

### 8.2 Decision

- **C++ is the reference** of every concept that has state: transport,
  discovery, the clients of telemetry, tuning and OTA, the log tables.
- **TypeScript is a client of the hub and nothing else.** The hub is the
  backend of the pages and of the VS Code extension; `gateway.proto` is the
  API between the two, made of interface-level messages, never raw frames.
  The front does only front: the hub carries the state (node table,
  telemetry tables and active streams, log tables, the OTA session, the
  profiles), and a page that closes and reopens gets it back. Telemetry
  enable, tuning set and list, log control, RC, reboot and scenario become
  gateway-local services like `OtaCommand`. This is the next rework, with
  its own issue; it absorbs section 9.4 (the hub subscribes to status and
  logs for its clients).
- **GDScript keeps its port**, minimal: the transport (keepalive, hops) and
  the part of discovery the plant needs (answer who it is, ask every node
  that appears, know the kinds). The GDExtension that would remove the port
  stays a documented option, not taken.
- **Dart becomes a full Dart node.** Native UDP transport
  (`RawDatagramSocket` with `broadcastEnabled`; the Wi-Fi multicast lock the
  app already holds is what lets Android deliver the incoming broadcasts,
  it is process-wide and serves a Dart socket as it served the C++ one),
  messenger dispatch by tag, discovery directory, the managers above. The
  NDK build, the shim and ffigen go. The reason: the BLoCs bind to manager
  streams, and a manager that mirrors C++ objects behind FFI is a binding
  layer for no Dart logic; a pure-Dart node is also testable on the host
  against the C++ reference the day tests resume.
- **Python ports nothing.** The campaign's node is replaced later by a C++
  campaign node linking the components, Python orchestrating processes only
  or going away; its own issue. Until then the Python node keeps sending
  and reading what still works (`Rc` and `SimScenario` unicast, `Status`
  broadcast) and is not touched.

### 8.3 Tests are frozen

From step 6b of this rework on, no new test is written: the existing suite
is kept green, adapted only where an API it exercises changed. Verification
across languages (a C++ reference peer, one conformance driver per port in
its language's test tool, `pnpm smoke` for the gateway API) is a subject of
its own, to be designed on its own, not built by the way.

## 9. Out of scope, and why

### 9.1 Threading the transport

Everything today moves during a `poll()` the application drives, single
threaded, and the flight loop pace is the sensor cadence. A threaded
transport (a TX queue drained towards the media, an RX queue dispatched to
handlers) is a later step; this design keeps the shape ready for it (the
messenger is the one caller of `poll()`, its RX side is where the queue
goes) and does not build it.

### 9.2 A peek before the decode

The relay used to compare one tag byte and decode only the envelopes it
answered; the messenger decodes every payload the transport delivers, LAN
broadcasts included, before finding that no handler claims the tag. On the
ESP32 this is a per-broadcast cost the tag peek did not have. The generic
fix belongs in the messenger, not in a node: read the body tag off the
first bytes (`envelopeBodyTag()` in `protocol/envelope.hpp` does it) and
skip the decode when no handler holds that tag. Not done in this rework;
`Messenger::unhandled()` counts what it would save.

### 9.3 A port layer for the clock

`poll(nowUs)` stays: the transport does not read a clock, like flight-core
and log. A port layer (clock, and later sockets and UART) that platform
independent components could depend on is a separate design.

### 9.4 Subscriptions for status, logs and telemetry

Telemetry already has one (`TelemetryEnable` / `TelemetryAck` /
`TelemetryData`, one active stream per drone). Status and logs do not: they
are broadcast. Issue #25 records the measured cost (about 39% loss on
broadcast out of the ESP32 on the bench) and proposes subscribe messages
for them. This rework makes those broadcasts impossible (section 4.3) and
leaves the subscription protocol to that issue, with the temporary
asymmetry of section 7.2 in between.
