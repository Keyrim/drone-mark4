# Communication stack - target design

Status: sections 3 to 6 are on `main` (issue #29, pull request #30, merged
2026-09-14). Section 2 describes the code as it was before that rework;
section 8 is the decision that shapes the rework after it (the gateway
API, the Dart and GDScript ports as the only ports); section 9 lists what
the first rework left out; section 10 is the design of the second rework,
decided on 2026-09-14, amended on 2026-09-15 with the decisions taken
before implementation, and being implemented on branch
`refactor/comm-stack-v3` (issue #31).
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
  gateway-local services like `OtaCommand`. This is the next rework,
  designed in section 10; it absorbs section 9.4 (the hub subscribes to
  status and logs for its clients).
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
  broadcast) and is not touched. Superseded by section 10.8: the Python
  tooling is removed at the start of the second rework.

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
goes) and does not build it. Section 10 keeps that seam: providers and
consumers hold a `Messenger&`, never a `Transport&`, read no clock and set
no timer.

### 9.2 A peek before the decode

The relay used to compare one tag byte and decode only the envelopes it
answered; the messenger decodes every payload the transport delivers, LAN
broadcasts included, before finding that no handler claims the tag. On the
ESP32 this is a per-broadcast cost the tag peek did not have. The generic
fix belongs in the messenger, not in a node: read the body tag off the
first bytes (`envelopeBodyTag()` in `protocol/envelope.hpp` does it) and
skip the decode when no handler holds that tag. Not done in this rework;
`Messenger::unhandled()` counts what it would save. Section 10 removes
every application broadcast, so the cost it would save goes away with them.

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
asymmetry of section 7.2 in between. Section 10.4 is that protocol.

## 10. Services on the messenger

Decided on 2026-09-14. This section is the design of the rework that
section 8.2 announced: the wire logic of every concept leaves the Apps and
the pages for components with one shape, the streams get a subscription,
the one-shot messages get an acknowledgement, and the gateway API becomes
typed messages. It absorbs sections 9.2 (not the peek, the counter it
would save), 9.4 and the temporary asymmetry of section 7.2. Sections 3 to
6 stay as they are; what this section changes in them is named where it
does.

### 10.1 Three roles, one directory per concept

Every concept that travels on the wire (log, telemetry, tuning, status,
OTA) is written once, in C++, as three classes with fixed names:

- **Provider**, on the node that owns the data. An `AbsMessageHandler`
  that answers requests towards `src` and emits its stream towards its
  subscribers. It holds the configuration of the concept: there is exactly
  one configuration per node, never one per consumer (10.4).
- **Consumer**, on a node that uses the data: the hub, the phone, the
  plant, later the campaign node. It pulls the tables, subscribes to the
  streams, keeps what it learnt, and drops all of it when the node goes
  down. It holds a `Messenger&` and a table sized by the composition.
- **Gateway**, in the hub only. It maps the commands of `gateway.proto`
  onto one Consumer and publishes what the Consumer holds as typed messages
  to the websocket clients (10.6).

"Client" disappears from the code: it named the consumer side while the
hub is the server of its own websocket clients, and the two readings met
in the same file.

One directory per concept under `software/components/`, holding every
role of that concept, on the model `ota/` already follows for its updater:

| directory | leaf targets, unchanged | new or moved targets |
|---|---|---|
| `log/` | `log` (the library), `log_wire` (codec helpers) | `log_provider` (the `TransportSink` route, the module table, `LogControl`, the line stream), `log_consumer` (tables and lines per node) |
| `telemetry/` | `telemetry` (the registry) | `telemetry_provider` (what `services/telemetry_service.hpp` is today), `telemetry_consumer` (table pull, stream) |
| `tuning/` | none | `tuning_provider` (links `flight_core`, what `services/tuning_service.hpp` is today), `tuning_consumer` (the parameter table per node, set and get) |
| `status/` | none | `status_provider` (`packStatus` and the publisher, today in `platform_common`), `status_consumer` (the last report per node) |
| `ota/` | `ota` (updater, store interface, boot policy) | `ota_provider` (what `services/ota_service.hpp` is today), `ota_consumer` (the hub's `OtaClient`, moved as it is) |

`services/` dissolves into these. A leaf stays a leaf: `log` and
`telemetry` still link `drone_warnings` alone. Every provider builds for
the F405 (no heap, no iostream, no exceptions). A consumer follows the same
rules: its tables are fixed arrays whose size is a template parameter the
composition chooses (`LogConsumer<Transport::MAX_NODES>` on the hub, the
only composition that consumes anything today; a board that consumes
something one day sizes it at what it can afford). A consumer is also an
`AbsDirectoryListener`: the kind of a node is only known from its
`Announce`, and the consumer alone knows which kinds carry its provider
(10.4), so it subscribes and pulls from `onIdentity()` and the hub names
no kind. One exemption: `ota_consumer` is `OtaClient` moved, not
rewritten. It reads a bundle from a filesystem and keeps its `std::string`
and `std::function`, so the target is declared for the desktop only, where
the one OTA consumer lives; its session state machine and its own retries
are untouched (10.2). The gateways are hub code and stay under
`software/hub/`, one file per concept.

`discovery/` and `messaging/` are not concepts: they are the floor every
concept stands on and keep their names.

### 10.2 Requests and acknowledgements

The transport's keepalive says who is reachable. It does not say whether a
given message arrived, and UDP loses some; the answer is an application
acknowledgement, in the messaging component, available to every node in
every direction.

**Wire.** `Envelope` gains one field outside the oneof: `uint32 request_id`
(field 100), 0 when absent. A message carrying a non-zero id is a request;
the `Ack {}` that acknowledges it carries the same id and nothing else: it
says "arrived", not what was done. Every node numbers its own requests from
its own counter, so a request is identified by the pair (peer node id,
request id).

**Messenger.** Two ways to send, chosen by the sender, never by the
receiver:

```
bool send(uint32_t dst, const mark4_Envelope &e);                          // once, no id: the streams
bool request(uint32_t dst, mark4_Envelope &e, RequestPolicy policy = {});  // numbered, kept, resent
void tick(uint64_t nowUs);                                                 // resends, gives up
struct RequestPolicy { uint64_t periodUs = 500'000; uint8_t retries = 5; };
```

`request()` is reached through a protected helper of `AbsMessageHandler`,
which forwards to the messenger with the handler as the owner. It stores
the encoded bytes (`MAX_ENVELOPE_SIZE` each) in a pending table the
composition hands to the messenger as a `std::span<PendingRequest>` over
an array the App owns: 4 entries on a board, 256 on the hub, where one
node appearing costs five requests at once. A span rather than a template
parameter or a constructor argument: without heap a fixed array must know
its size at compile time, and a span keeps one `Messenger` type for every
composition, the way a handler already hands the messenger a span over
its tags. `tick()` resends an unanswered request every `policy.periodUs`,
up to `policy.retries` sends, then drops it and tells its owner
(`AbsMessageHandler::onRequestFailed(dst, requestId)`, a virtual with an
empty default). The policy is per call, with the defaults above: a reboot
and a table page do not want the same values. A request to a node the
transport does not know is refused at once, not kept. A full table refuses
the request and counts it. A node that goes down takes its pending
requests with it, each reported failed.

**The acknowledgement is the messenger's.** On receiving a message that
carries an id, the receiving messenger sends `Ack` back to `src` before
dispatching, whether or not a handler claims the tag. It is not the
handler's business, so nothing is forgotten, and a `Reboot` is
acknowledged before the reset rather than never. A node asked something it
does not understand acknowledges and drops it; not asking it is the
consumer's job (10.4). The tag `ack` belongs to the messenger: an `Ack`
whose (src, id) matches a pending request completes it, any other is
counted and dropped.

**An answer that matters is a request too.** The acknowledgement covers
the request, not what comes back for it. A page of a table, a
configuration as applied, a `TuningAck`: the provider sends each with
`request()`, and the consumer's messenger acknowledges it in turn. Four
messages where a bare RPC would use two; the extra ones are 3-byte
acknowledgements on exchanges that happen once per boot or by hand. An
answer correlates by its content (the cursor of a page, the id of a
parameter, the configuration itself), never by a request id.

**Presence reaches the handlers through the messenger.** The messenger is
the one `AbsPresenceListener` of the messaging side: it drops the pending
requests of a node that goes down and relays both events to every handler
(`AbsMessageHandler::onNodeUp(id)` / `onNodeDown(id)`, virtuals with empty
defaults). A provider forgets a subscriber there, a consumer its tables.
The transport's own listener table (4 entries) is not consumed by the
concepts; `DiscoveryDirectory::MAX_LISTENERS` goes from 4 to 8, the hub
holding four consumers and its node table.

These are the constants and the retry logic `DiscoveryDirectory`
(`IDENTITY_TIMEOUT_US`, `IDENTITY_RETRIES`) and the hub's telemetry pull
(`TELEMETRY_RETRY_US`, `TELEMETRY_MAX_ATTEMPTS`) each wrote by hand; both
move onto `request()`.

**QoS 1, at least once.** A request lost is sent again; a request whose
acknowledgement was lost is sent again and applied twice. So every request
is an idempotent state: "subscribe me", "this is the configuration", "set
this parameter to this value", "reboot", "here is page 3". A message that
would not survive being applied twice does not go through `request()`. The
OTA session is not concerned: it has its own go-back-N over `OtaChunk` /
`OtaChunkAck` and its own state machine (`docs/ota-design.md`), unchanged.

**One-shots and streams.** Every one-shot message is a request, whatever
its direction: a consumer asking a provider, a provider answering or
notifying its subscribers (10.4), a node asking another node's identity
and the `Announce` that answers. Stream data (`Status`, `TelemetryData`,
`Log` lines, `Rc`) goes by `send()`: no id, never acknowledged, never
resent. A sample is only worth its own instant, and the subscription is
what guarantees the stream as a whole.

### 10.3 The keepalive carries the boot id

A node's id is stable across reboots (section 3 of
`transport/README.md`), so a node that reboots in under `NODE_EXPIRY_US`
never leaves the tables of its peers: its subscribers believe they are
subscribed, its providers have forgotten them. The sequence number does
not catch it reliably (a backward jump is caught, a reboot whose first
frames are lost is not), and a field on every frame would cost every frame.

The keepalive, which is the transport's own once-a-second message and today
an 11-byte header alone, gains a 4-byte payload: `boot u32`, little-endian,
drawn at random when the transport is constructed (the hardware RNG of the
F405, `esp_random()` on the ESP32, the process's random source on desktop;
never anything derived from the chip UID, which is what makes the id
stable; the composition draws it and hands it to the transport's
constructor, which depends on nothing). The keepalive is marked in the
header: the last header byte, `hops`, splits into the hop count on its low
nibble (`MAX_HOPS` is 4) and flags on its high nibble, bit 7 being the
keepalive flag. A frame with the flag is the transport's own, whatever its
payload size, and is never delivered upward; a frame without it is an
application payload. The payload size alone could not tell them apart
(`TelemetryListRequest{cursor: 1}` encodes in exactly 4 bytes) and the
transport reads no protobuf. Every frame sent today already reads as flags
0, so the bytes on the wire are compatible until a node sends its first
keepalive. Rules:

- The transport keeps `boot` per node. A frame from an unknown node
  creates its entry with `boot = 0` (not known yet); the first keepalive
  sets it without an event.
- A keepalive whose `boot` differs from a known non-zero one is a new
  incarnation of the same id: the transport fires `onNodeDown` then
  `onNodeUp` for it, resets its counters, and every listener does what it
  does for a node that left and came back (consumers drop their
  subscriptions, tables and pending requests, then start over; providers
  drop the subscriber).
- The first `poll()` of a node sends its keepalive, so a rebooted node
  announces its incarnation before, or within one poll of, its first data
  frame. A data frame that arrives in that window is delivered under the
  old incarnation and is harmless: the down/up that follows resets
  everything.
- `RESYNC_THRESHOLD` keeps its one role, the loss counter.

Cost: 4 bytes per node per second. Collision: one in four billion. Three
codecs to change, all ours: `transport.cpp`, `transport.gd`, the Dart
frame codec, each masking the hop count and reading the flag. `Node` gains `boot`, and the gateway's `Node` does not: the
clients see the effect (a node that goes down and up), not the mechanism.

### 10.4 Subscriptions

**Binary, per stream.** A stream is subscribed or not; nothing about it is
per subscriber. Each stream has one request, `XxxSubscribe { bool
enabled }`, answered like every configuration request by the same message
as applied. The provider keeps a `SubscriberTable<N>`: N node ids, add,
remove, iterate to emit, remove on `onNodeDown`. The stream is emitted
while the table is not empty and to every entry; a full table refuses the
subscribe, and the answer says so with `enabled = false`. N is a constant of each
stream, small because a board's UART pays every entry: `Status` 4, `Log`
lines 2, `TelemetryData` 2. One emission per subscriber today; when the
transport learns multicast, one emission, and nothing above the transport
changes. That is the reason the subscription carries no parameter.

**Configuration lives in the provider, once per node.** What a stream
carries and how often is a state of the node, not of a consumer:

- `Status`: no configuration. The node emits it every
  `STATUS_PERIOD_FRAMES` frames, a constant of the composition.
- Telemetry: `TelemetryConfig { repeated uint32 ids; uint32 period_ms }`,
  a request that replaces the enabled set and the period wholesale, last
  writer wins, answered by the configuration as applied (ids kept, period
  clamped). `TelemetryEnable` and `TelemetryAck` disappear.
- Log lines: the configuration is the module table, `LogControl.set`
  moves one level; answered by the `LogModuleInfo` of that module as it
  stands.
- Tuning has no stream; `TuningSet` is a configuration request like any
  other, answered by `TuningAck`.

**A configuration change is told to every subscriber.** Subscribing to a
stream means receiving its data and its configuration changes. When a
request changes the configuration, the requester gets the applied
configuration as its answer, and every other entry of the stream's
`SubscriberTable` gets the same message; both are requests of the
provider's own (`request()`, acknowledged on arrival, resent and given up
on like any request, 10.2). Two hubs on two machines watching the same drone therefore agree
on its log levels the moment one of them moves one. A consumer that pulled
a table without subscribing to the stream is not told: a table without its
stream has no reason to stay exact. This is the one unsolicited emission
in the system besides stream data, and it only ever goes to a subscriber:
section 6's rules stand.

**Who subscribes to what.**

| consumer | Status | Log lines | TelemetryData |
|---|---|---|---|
| hub | every `FIRMWARE` and `DRONE_SIM` of a matching wire hash, always | every `FIRMWARE`, `DRONE_SIM`, `RELAY` and `GATEWAY`, always | while at least one websocket client holds it (10.6) |
| phone | its drone | no | no |
| plant | each drone it hosts (the overlay) | no | no |
| campaign node (later) | its drones | as it needs | as it needs |

The kinds are the consumer's own constants, checked in its `onIdentity()`:
they name the nodes that carry the matching provider (the plant and the
phone carry no `LogProvider`, the relay no `StatusProvider`), so no request
is ever sent to a node that would acknowledge it and drop it.

**Run stats.** `SimRunStats` goes. Its one reader was the campaign tool
(10.8), and a one-shot republished every 50 frames for late joiners has no
place in a system of requests. The sim's run tracker keeps computing the
hash of every run and logs it when the run seals; the future test harness
reads it there.

### 10.5 Tables and pages

Three tables are pulled today in three shapes (`TelemetryDescriptors`:
total, cursor, 6 descriptors; `LogModules`: start index, total, 8
modules; `TuningInfo`: one description per `pump()`). They become one
shape: a page request `{ uint32 cursor }` sent with `request()`, a page
answer `{ uint32 total; uint32 cursor; repeated T items }` sized to the
frame and sent with `request()` too (10.2). `TelemetryListRequest` stays;
`LogModulesRequest` and `TuningListRequest` join it with the same one
field; `LogModules` renames `start_index` to `cursor`; a new `TuningInfos`
replaces `TuningInfo` as a body, and `TuningInfo` loses `index` and
`count`. `LogControl` and `LogModuleLevel` go: the level move is
`LogSetLevel { module_id, level }`, answered by the module's
`LogModuleInfo` as a body of its own. The provider's `pump()` goes: the
consumer paces the walk by asking one page at a time, which is what the
UART wanted. Every tag removed from `Envelope` is `reserved`.

A consumer walks a table with `TablePull<T, MAX>`: one `request()` per
page, cursor advanced on each answer, complete when `cursor + items ==
total`, abandoned when a request fails (the default policy: five sends at
500 ms, about the three seconds of the node expiry), restarted from zero
on `onNodeUp`. The ids of a table are only stable while the node runs, so
a reincarnation (10.3) drops the table and the consumer pulls it again.

### 10.6 The gateway API

`gateway.proto` keeps its rules (one `GatewayMessage` per websocket
message, one nanopb union, anything per node and unbounded in a message of
its own, `id` echoed on the `Ack`) and changes its content: **`Frame`
disappears in both directions.** The hub decodes everything it hears
through its consumers and publishes typed messages; a client sends typed
commands and never an encoded `Envelope`. The pages keep the generated
`mark4_pb.ts` for the types `gateway.proto` imports (`Status`,
`TelemetryDescriptor`, `LogModuleInfo`, `Announce`, `Rc`, `TuningAck`) and
nothing else.

Client to gateway, every one a command the gateway carries out through one
consumer and answers with `Ack` when `id` is set:

| body | fields | what the gateway does |
|---|---|---|
| `TelemetryCommand` | `node`, oneof `config { ids, period_ms }` / `subscribe { bool }` | `config`: `TelemetryConfig` to the node, last writer wins between clients too. `subscribe`: marks this client; the gateway subscribes to the node while at least one client is marked, unsubscribes when the last one clears or disconnects |
| `LogCommand` | `node`, oneof `refresh {}` / `set_level { module_id, level }` | pulls the table again, or one `LogControl.set` |
| `TuningCommand` | `node`, oneof `refresh {}` / `set { id, value }` / `get { id }` | pulls the table again, or one request |
| `PilotInput` | `node`, `Rc` | forwards one `Rc` to the node from the gateway's own id (10.7) |
| `NodeCommand` | `node`, oneof `reboot {}` / `scenario { SimScenario }` | one request |
| `OtaCommand`, `ProfileCommand` | unchanged | |

Gateway to client, the state the gateway holds, on every change and whole
to a client that connects:

| body | what |
|---|---|
| `NodeTable` | as today, minus `Node.log_modules` (the TODO of `Node`) |
| `NodeLogModules { node, modules }` | one node's whole module table |
| `NodeLogLines { node, repeated Log }` | lines as they arrive, and on connect the last `LOG_RING` lines the gateway kept per node (256), oldest first |
| `NodeStatus { node, Status }` | the last report of a node, at the node's cadence |
| `NodeTelemetry { node, descriptors }` | as today |
| `NodeTelemetryConfig { node, ids, period_ms, subscribed }` | the configuration as the node applied it, and whether the gateway holds the stream |
| `TelemetrySamples { node, TelemetryData }` | the stream, to the clients marked on that node |
| `NodeTuning { node, repeated TuningInfo }` | one node's parameter table |
| `TuningResult { node, TuningAck }` | the answer to a set or a get, to every client (they correlate on `GatewayMessage.id`) |
| `GatewayStatus`, `OtaState`, `ProfileList`, `Profile`, `Ack` | unchanged |

The gateway's own log lines take the same route as everyone's: its
`LogProvider` has a local sink next to its subscriber table (an
`AbsLogSink` fed after the rate limit, since a messenger cannot send to
its own node), the log gateway is that sink, so the lines appear as
`NodeLogLines` from its own node id, and a second hub that subscribes to
them gets them on the wire like any node's.

The VS Code extension and `pnpm smoke` are two more clients of this API
and change with it.

### 10.7 RC through the gateway

The gateway is the pilot node: it forwards each `PilotInput` as one `Rc`
to the named node, from its own id, at the cadence the client sends. Two
rules the fail-safe depends on:

- **It never repeats.** A client that stops sending stops the stream, and
  the node's RC timeout does the rest. The gateway holds no last state to
  resend.
- **One pilot per node at a time.** The first client to send takes the
  seat; another client's input is refused (`Ack`) while the seat is held;
  the seat is released when the holder disconnects or has sent nothing for
  `RC_PILOT_WINDOW_US`. `GatewayStatus.rc_clients` counts the seats held.

### 10.8 Scope, removals, the other languages

- **What stops broadcasting, for good**: `status_publisher.hpp`, the
  `TransportSink` route of every node, the `LogModules` pages published at
  boot. `SimRunStats` is removed altogether (10.4).
  `platform_common/envelope_io.hpp` and its `sendEnvelope()` are deleted;
  the drone Apps no longer hold a `GATEWAY`-kind destination, and the
  asymmetry of section 7.2 is gone. `BROADCAST_NODE` is refused by the
  messenger as section 4.3 says, and nothing asks for it any more.
- **Python goes.** `tools/batch/`, `tools/telemetry_wire.py`, the
  `proto_py` target of `protocol/CMakeLists.txt`, the two batch steps of
  `ci.yml`, and every mention of the campaign in the documentation. The
  build scripts under `scripts/` (`build_app.py`, `run_app.py`,
  `make_ota.py`, `gen_godobuf.py`) import nothing from it and stay. What
  goes with it: the determinism check (`--verify-repro`) CI ran on every
  push; it comes back with the test harness of section 8.3, which starts
  the real processes (hub, drone_sim, Godot, the phone's host build) and is
  designed on its own.
- **GDScript** ports the minimum: the keepalive boot id and flag in
  `transport.gd`, a request helper (`request_id`, per-call policy, resend,
  give up, and the automatic `Ack` on every numbered message received:
  what `discovery.gd` does by hand today, once), and a `StatusConsumer`
  (subscribe to each hosted drone, read the stream). The plant stops
  reading `Status` off broadcasts.
- **Dart** mirrors the same three, constant for constant: the boot id and
  flag in the frame codec, `PendingRequests` and the automatic `Ack` in
  its messenger, `StatusConsumer` in `DroneManager`. `PilotManager` does
  not change: `Rc` is a stream.
- **Firmware and relay** carry the providers of what they have (the relay:
  `LogProvider` and its `Discovery`); nothing else changes for them.
- **Tests stay frozen** (section 8.3): the existing suite is adapted where
  an API it exercises changed, nothing new is written.

### 10.9 Order

The Python removal comes first so CI is green at every step after it.

1. Python removal: `tools/`, `proto_py`, `ci.yml`, documentation.
2. messaging: `request_id`, `request()` / `tick()`, `RequestPolicy`, the
   pending table, `Ack` on arrival, `onRequestFailed()`, presence relayed
   to the handlers; `DiscoveryDirectory` on it. transport: the boot id and
   the flag in the keepalive, the reincarnation event. The GDScript and
   Dart frame codecs take the flagged 4-byte keepalive in the same step
   (they only have to accept it to keep hearing the C++ nodes).
3. `mark4.proto`: the subscribes, `TelemetryConfig`, the unified pages,
   `TuningInfos`, `LogSetLevel`, `SimRunStats` removed. `WIRE_HASH` moves
   once.
4. The concept directories with their providers; `services/` and
   `platform_common`'s wire files dissolve; drone_sim, the firmware and the
   relay on them. From here the hub hears nothing until step 5.
5. The consumers and `TablePull`; the hub on them (the `Reader` and the
   pull maps of `HubApp` go).
6. `gateway.proto` v2 and the gateways in the hub; the pages, the
   extension and `pnpm smoke` on the typed API; `Frame` gone.
7. GDScript: request helper, `StatusConsumer`.
8. Dart: `PendingRequests`, `StatusConsumer`.
9. Documentation: this document's status line, `target-architecture.md`,
   the component READMEs, `docs/mobile-app.md`, `sim-godot/README.md`,
   `CLAUDE.md`.

What is unusable between steps: the bench pages from step 4 to step 6, the
plant's overlay from step 3 to step 7 (the lockstep link itself is not
touched), the phone's cockpit from step 3 to step 8.
