# Transport health - design

Decided on 2026-09-19. The state of the transport layer of the whole
system, seen from every node, gathered by the hub and shown as a graph in
the editor: which nodes are on the wire, over which links, how many frames
each one receives and loses from each other, and a verdict that says
whether the transport is healthy and, when it is not, why.

This document is the closed specification of the work: the subagents that
implement it take no decision of their own. What is not written here is
asked, not guessed. The code is the reference once merged; where the code
had to depart from this document, section 9 says so.

## 1. Why

Every node's transport keeps a table of its peers with `received`, `lost`,
`duplicates`, `hops` and `lastSeenUs` per peer, and global send counters
(`sent`, `sentBytes`, `refused`, `dropped`, `relayed`). Every messenger
keeps its own (`undecodable`, `unhandled`, `requests`, `resent`,
`completed`, `failed`, `unmatchedAcks`). Today only the hub's table is
visible (`NodeTable` of `gateway.proto`), and only from the hub's point of
view.

A loss on the direction A to B is only seen by B. The hub's table shows
the losses towards the hub and never the losses towards the board (hub,
relay, UART), which are exactly the ones that make requests fail. The
feature is therefore: every node reports its own view of the transport,
the hub gathers every view into the directed matrix (observer, peer), and
derives from it what a person needs to know.

## 2. Principles

- Nothing travels unasked. A node reports only to the nodes that
  subscribed, and the hub subscribes only while a websocket client asked
  for the reports (the transport page is open). With no client marked, the
  hub's own view is the whole picture, and it costs the wire nothing.
- Counters travel cumulative, never as rates: a lost report skews nothing,
  the hub computes the deltas.
- The hub is provider and consumer of the concept: it reports to another
  gateway that subscribes, and it feeds its own consumer through a local
  path since a messenger cannot send to its own node.
- The GDScript plant and the Dart phone do not report in this version.
  They still appear as peers in everyone else's reports, with only their
  inbound direction known, and the page draws them as such.
- No new unit test is written; the existing tests are adapted where an API
  they exercise changes. Candidates for the testing design are listed in
  section 8.
- Every rule of `CLAUDE.md` holds: English, ASCII, `mark4` namespace, no
  heap in the components, Doxygen, Conventional Commits, the log library
  for every diagnostic line.

## 3. Transport additions (`software/components/transport/`)

### 3.1 `AbsLink` (`transport/link.hpp`)

```cpp
/// What medium a link is: what a report says of it, and what a page draws.
enum class LinkKind : std::uint8_t
{
    UART = 1, ///< a point-to-point serial line behind the serial framing
    UDP = 2,  ///< an IPv4 LAN, one shared discovery port and one data socket
};

/// @return what medium this link is
[[nodiscard]] virtual LinkKind kind() const = 0;

/// @return frames the medium could not deliver whole, cumulative: a CRC
///         failure or an impossible length on a serial line, a datagram
///         larger than the caller's buffer on UDP
[[nodiscard]] virtual std::uint32_t rxErrors() const = 0;
```

`UartLink::kind()` returns `UART`; `UdpLink::kind()` returns `UDP`.

`UartLink::rxErrors()` is the parser's count plus the frames dropped for
being larger than the caller's capacity in `receive()`. `SerialFrameParser`
gains a `std::uint32_t m_errors` counter and an accessor `errors()`,
incremented on a CRC mismatch and on an impossible announced length (0 or
greater than `SERIAL_MAX_PAYLOAD`), the two places where the parser goes
back to `SYNC0` without returning a frame. A stray byte while hunting for
the sync pair is not an error: that is the normal resynchronization.

`UdpLink::rxErrors()` counts the oversized datagrams `ReadOne()` skips
(`received > capacity`); the counter is a member of the link and `ReadOne`
becomes a non-static member (or takes the counter by reference; the
implementer picks the smaller change).

The test fake `software/tests/unit/recording_link.hpp` (`RecordingLink`)
implements both: `kind()` returns `LinkKind::UDP`, `rxErrors()` returns 0.
Nothing else implements `AbsLink` outside `transport/`.

### 3.2 `Transport` (`transport/transport.hpp`)

Per-link counters, indexed like `m_links`:

```cpp
/// What crossed one link, in both directions, cumulative.
struct LinkStats
{
    std::uint32_t framesIn = 0U;  ///< frames the link handed over, header decoded or not
    std::uint32_t bytesIn = 0U;   ///< their bytes, header included
    std::uint32_t framesOut = 0U; ///< frames the link took: this node's sends,
                                  ///< its keepalives and what it relayed
    std::uint32_t bytesOut = 0U;  ///< their bytes, header included
    std::uint32_t refused = 0U;   ///< frames the link would not take (a full UART ring)
};

[[nodiscard]] std::size_t linkCount() const;
[[nodiscard]] const AbsLink &link(std::size_t index) const;      // 0 <= index < linkCount()
[[nodiscard]] const LinkStats &linkStats(std::size_t index) const;
```

Rules:

- `poll()` counts every frame a link's `receive()` returns in `framesIn`
  and `bytesIn` of that link, before `onFrame()` looks at it.
- Every `send()` / `broadcast()` call on a link, from `send()`,
  `sendKeepalive()` and `relay()`, counts in `framesOut` and `bytesOut`
  (the whole frame size) when the link returned true, in `refused` when it
  returned false. The existing global counters keep their meaning exactly
  (`sent()` and `refused()` describe this node's own sends and keepalives,
  `relayed()` counts the forwards whether the link took them or not).

Two churn counters on the transport:

```cpp
/// @return peers forgotten for silence (NODE_EXPIRY_US), cumulative
[[nodiscard]] std::uint32_t expired() const;
/// @return peers seen restarting (another boot id), cumulative
[[nodiscard]] std::uint32_t restarted() const;
```

`expire()` increments the first per node forgotten; `onBootId()` increments
the second when it fires `notifyDown` then `notifyUp`.

The GDScript and Dart ports are not touched: they neither report nor need
the new counters.

`transport/README.md` documents the new counters (API section, Links
section) and gains a section on the provider and the consumer (section 4
below).

## 4. Wire (`software/components/protocol/mark4.proto`)

Three bodies in the `Envelope` oneof, numbered after the last one in use:

```proto
TransportSubscribe transport_subscribe = 54;
TransportSubscription transport_subscription = 55;
TransportReport transport_report = 56;
```

Messages, placed after the tuning family with the same comment style as
the other concepts:

```proto
// The transport concept: what every node's transport and messenger count.
// Request, answered by the subscription as held (TransportSubscription).
message TransportSubscribe {
  bool enabled = 1;
}

// The subscription as the provider holds it.
message TransportSubscription {
  bool enabled = 1;
}

enum LinkKind {
  LINK_KIND_UNSPECIFIED = 0;
  LINK_UART = 1;  // a point-to-point serial line
  LINK_UDP = 2;   // an IPv4 LAN
}

// One physical link of the reporting node, cumulative counters.
message TransportLink {
  LinkKind kind = 1;
  uint32 frames_in = 2;
  uint32 bytes_in = 3;
  uint32 frames_out = 4;
  uint32 bytes_out = 5;
  uint32 refused = 6;    // frames the medium would not take (a full UART ring)
  uint32 rx_errors = 7;  // frames the medium could not deliver whole (CRC, length, oversize)
}

// One peer as the reporting node's transport holds it.
message TransportPeer {
  uint32 id = 1;
  uint32 link = 2;        // index into TransportReport.links
  uint32 hops = 3;        // relays the last frame from it crossed
  uint32 received = 4;
  uint32 lost = 5;
  uint32 duplicates = 6;
  uint32 age_ms = 7;      // since the last frame from it
}

// Stream: one node's view of the transport, every second, to the nodes
// that subscribed. Cumulative counters, so a lost report skews nothing.
// The peer table travels by pages of at most 4 (peer_cursor, peer_total),
// every page carrying the counters again; a consumer takes the counters
// from any page and replaces the peer slice the page names.
message TransportReport {
  // this node's transport
  uint32 sent = 1;            // frames this node handed to a link: its sends and keepalives
  uint32 sent_bytes = 2;      // their payload bytes
  uint32 refused = 3;         // sends that reached no link
  uint32 dropped = 4;         // frames dropped: too short, table full, nobody to relay to
  uint32 relayed = 5;         // frames forwarded onto another link
  uint32 expired = 6;         // peers forgotten for silence
  uint32 restarted = 7;       // peers seen restarting (another boot id)
  // this node's messenger
  uint32 undecodable = 8;     // payloads that were no Envelope
  uint32 unhandled = 9;       // messages nobody claimed
  uint32 requests = 10;       // requests started
  uint32 resent = 11;         // requests sent again
  uint32 completed = 12;      // requests acknowledged
  uint32 failed = 13;         // requests given up on
  uint32 unmatched_acks = 14; // acknowledgements matching no pending request
  repeated TransportLink links = 15;  // at most 4, in declaration order
  uint32 peer_cursor = 16;    // index of peers[0] in the node table
  uint32 peer_total = 17;     // peers in the whole table
  repeated TransportPeer peers = 18;  // at most 4 per page
}
```

`mark4.options`:

```
mark4.TransportReport.links         max_count:4
mark4.TransportReport.peers         max_count:4
```

Worst case of one page encoded: about 450 bytes, under `MAX_PAYLOAD` (512),
and the struct stays under `MAX_ENVELOPE_STRUCT_SIZE` (400); both are
checked by the build (`static_assert` in `protocol/envelope.hpp`, the
nanopb `_size` constant). If either limit is hit, the implementer stops
and reports rather than lowering a bound.

No `reserved` statement (the godobuf parser cannot read it). The wire hash
changes: every node rebuilds, which is the normal course of a schema
change. The GDScript and Dart codecs regenerate from the schema at build
time and their messengers acknowledge and drop the tags they do not claim.

`protocol/README.md` lists the three messages with the other concepts.

## 5. The concept in `transport/`: provider and consumer

Same directory as the leaf, on the model of `log/` (the leaf `log`, then
`log_provider` and `log_consumer` next to it). Two header-only targets,
each header with its one-include source in `src/` (the rule of PR #33):

- `transport_provider`: `include/transport/provider.hpp`,
  `src/provider.cpp`; links `messaging` and `log` PUBLIC, `drone_warnings`
  and `drone_strict` PRIVATE.
- `transport_consumer`: `include/transport/consumer.hpp`,
  `src/consumer.cpp`; links `messaging`, `discovery` and `log` PUBLIC,
  `drone_warnings` and `drone_strict` PRIVATE.

Log module ids in `log/module_ids.hpp`:

```cpp
inline constexpr std::uint16_t LOG_MODULE_TRANSPORT_PROVIDER = 27U; ///< transport/provider
inline constexpr std::uint16_t LOG_MODULE_TRANSPORT_CONSUMER = 28U; ///< transport/consumer
```

### 5.1 `TransportProvider`

```cpp
class TransportProvider final : public AbsMessageHandler
{
  public:
    static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_transport_subscribe_tag};
    static constexpr std::uint64_t REPORT_PERIOD_US = 1'000'000U;
    static constexpr std::size_t MAX_SUBSCRIBERS = 2U;
    static constexpr std::size_t PEERS_PER_PAGE = 4U;   // = the nanopb bound

    /// @param messenger the subscribes come from it, the reports leave by it,
    ///        and its counters are part of the report
    /// @param transport what is reported; must outlive the provider
    TransportProvider(Messenger &messenger, const Transport &transport);

    /// @brief Sends the report when due, to every subscriber: one page
    ///        per PEERS_PER_PAGE peers, all pages in the same call. With no
    ///        subscriber nothing is packed at all.
    /// @param nowUs current instant [us], from the caller's clock
    void tick(std::uint64_t nowUs);

    /// @brief Fills one page of the report, as tick() sends it. Public so a
    ///        node that consumes its own report (the gateway) can walk the
    ///        pages without the wire.
    /// @param cursor index of the first peer of the page
    /// @param nowUs current instant [us], for the ages
    /// @param[out] out the page
    /// @return peers written, 0 when cursor is past the table (the page
    ///         still carries the counters and peer_total)
    std::size_t fillPage(std::uint32_t cursor, std::uint64_t nowUs, mark4_TransportReport &out) const;

    bool onMessage(std::uint32_t src, const mark4_Envelope &envelope, std::uint64_t nowUs) override;
    void onNodeDown(std::uint32_t nodeId) override;

    [[nodiscard]] std::size_t subscribers() const;
    [[nodiscard]] std::uint32_t reportsSent() const;   // pages sent
};
```

Behaviour, copied from `StatusProvider` where it applies:

- `onMessage` on `transport_subscribe`: add or remove `src` in a
  `SubscriberTable<MAX_SUBSCRIBERS>`; a full table is a WARN of
  `transport/provider` and `applied = false`; answer with
  `TransportSubscription { enabled = applied }` sent as a `request()`.
- `onNodeDown`: remove the node, INFO line like the status provider's.
- `tick`: nothing with no subscriber. Otherwise when `nowUs -
  m_lastReportUs >= REPORT_PERIOD_US` (the first tick sends at once):
  `cursor = 0; do { fillPage(cursor, nowUs, page); send to every
  subscriber with Messenger::send(); cursor += page.peers_count; } while
  (cursor < page.peer_total);`. A stream: `send()`, never `request()`.
- `fillPage`: the transport counters and the messenger counters
  (`accessMessenger()` gives the messenger; every counter it exposes is a
  const accessor), `links` from `linkCount()` / `link(i).kind()` /
  `linkStats(i)` / `link(i).rxErrors()`, `peer_total = nodeCount()`,
  `peers` from `node(cursor + k)` for `k < PEERS_PER_PAGE`, `age_ms =
  (nowUs - min(nowUs, lastSeenUs)) / 1000`.

### 5.2 `TransportConsumer`

Non-template base over a storage span plus `TransportConsumer<N>`, exactly
the shape of `status/consumer.hpp` (read it first and copy its structure:
listener class with attach/detach, `Entry` table as a dense prefix, `open`,
`lookup`, `onNodeDown`, `onRequestFailed`, `Module()`).

```cpp
class AbsTransportConsumerListener
{
    /// @brief One node's report, pages merged: fired when the last page of
    ///        a report arrived.
    virtual void onReport(std::uint32_t nodeId, const TransportConsumerBase::Entry &entry, std::uint64_t nowUs) = 0;
    /// @brief A node went down: everything the consumer held of it is gone.
    virtual void onForgotten(std::uint32_t nodeId) = 0;
};

class TransportConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
{
  public:
    static constexpr std::array<pb_size_t, 2> TAGS = {mark4_Envelope_transport_report_tag,
                                                      mark4_Envelope_transport_subscription_tag};
    /// Kinds that carry a TransportProvider in this version.
    static constexpr std::array<mark4_NodeKind, 4> KINDS = {FIRMWARE, DRONE_SIM, RELAY, GATEWAY};
    static constexpr std::size_t MAX_LISTENERS = 2U;
    /// Peers one report may hold once its pages are merged: a transport's table.
    static constexpr std::size_t MAX_PEERS = Transport::MAX_NODES;

    struct Entry
    {
        std::uint32_t id = 0U;
        bool local = false;                  ///< this node's own report, fed by accept(), never subscribed
        bool subscribed = false;             ///< the node took the subscribe
        std::uint32_t subscribeRequest = 0U; ///< subscribe (or unsubscribe) waiting for its answer, 0 none
        bool hasReport = false;              ///< a complete report arrived
        mark4_TransportReport last;          ///< counters and links of the last complete report; its peers field is unused
        std::array<mark4_TransportPeer, MAX_PEERS> peers{}; ///< the merged peer table of that report
        std::size_t peerCount = 0U;
        std::uint64_t receivedUs = 0U;       ///< instant the last complete report arrived [us]
        std::uint32_t reports = 0U;          ///< complete reports from this node
        // staging of the report being received
        std::array<mark4_TransportPeer, MAX_PEERS> staging{};
        std::size_t stagingCount = 0U;
        bool staging_valid = false;          ///< a page with cursor 0 opened the staging
    };

    TransportConsumerBase(Messenger &, DiscoveryDirectory &, std::span<Entry>);
    [[nodiscard]] bool init() const;

    /// @brief Asks every node open now and later for its reports, or stops
    ///        asking: one TransportSubscribe per remote entry, as a request.
    ///        Turning it off also forgets the reports held of the remote
    ///        entries (hasReport = false), so nothing stale is published;
    ///        the local entry keeps its own.
    void setWanted(bool wanted);
    [[nodiscard]] bool wanted() const;

    /// @brief Opens the entry of this node's own report, fed by accept()
    ///        and never subscribed to. Once, by the composition.
    /// @return false when the table is full
    bool openLocal(std::uint32_t id);

    /// @brief Takes one page of one node's report: the counters are copied,
    ///        the peer slice [peer_cursor, peer_cursor + n) replaces the
    ///        staging; when the page completes the table (cursor + n >=
    ///        peer_total, or peer_total == 0) the staging becomes the
    ///        report, hasReport is set, reports is counted and the
    ///        listeners hear onReport(). A page with cursor 0 always opens
    ///        a new staging; a page whose cursor is not where the staging
    ///        ended is dropped (a lost page: the next report starts over).
    void accept(std::uint32_t nodeId, const mark4_TransportReport &page, std::uint64_t nowUs);

    void onIdentity(const DirectoryEntry &) override;   // open on a KINDS kind with no wireMismatch; subscribe if wanted
    void onForgotten(std::uint32_t) override;           // nothing: presence does it
    bool onMessage(...) override;                       // report page -> accept(); subscription -> entry state
    void onNodeDown(std::uint32_t) override;            // drop the entry, tell the listeners
    void onRequestFailed(std::uint32_t, std::uint32_t) override;  // WARN like the status consumer

    [[nodiscard]] const Entry *find(std::uint32_t id) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] const Entry &entry(std::size_t index) const;
};

template <std::size_t N> class TransportConsumer final : public TransportConsumerBase { ... };
```

The `TransportSubscription` answer sets `entry.subscribed = enabled` and
clears `subscribeRequest`. `setWanted(false)` sends `TransportSubscribe {
enabled = false }` to every subscribed remote entry. An `Entry` weighs
about 2.2 kB; `TransportConsumer<Transport::MAX_NODES>` on the hub is
about 70 kB, which is the hub's business.

### 5.3 Compositions

Every C++ node gets a provider, declared after its `LogProvider` and
ticked where its messenger is polled:

- `drone_sim` (`software/drone_sim/drone_sim_app.hpp` / `.cpp`):
  `mark4::TransportProvider m_transportProvider{m_messenger, m_transport};`
  ticked with `m_clock.nowUs()` in the run loop next to the updater's
  tick.
- firmware (`software/drone_firmware/firmware_app.hpp` / `.cpp`): member
  after the log provider, ticked in `pollTransport(nowUs)` after
  `m_messenger.poll(nowUs)`. It builds for the F405 as it stands (no heap,
  `drone_strict`).
- relay (`esp32-bridge/main/relay.cpp`): a member of `Relay` after
  `logProvider`, ticked in the loop after `messenger.poll()`;
  `esp32-bridge/main/CMakeLists.txt` adds
  `${DRONE_TRANSPORT}/src/provider.cpp` to the sources like
  `log/src/provider.cpp`. The relay's `STATS.debug` line stays.
- hub: section 6.

The pending request tables are untouched: the provider sends the answer to
a subscribe as one request, which the board's table of 4 affords.

## 6. The hub (`software/hub/`)

### 6.1 `gateway.proto`

```proto
// in the GatewayMessage oneof
TransportCommand transport_command = 55;
NodeTransport node_transport = 67;
TransportHealth transport_health = 68;
```

`Node` loses `received`, `lost` and `duplicates`: the three numbers become
`reserved 5, 6, 7;` with the comment "were the gateway's own receive
counters, which live in NodeTransport now; never reuse". `NodeTable` is
identity and presence only: id, address, port, `last_seen_ms_ago`,
`announce`.

```proto
// Client to gateway: this client wants the transport reports of every
// node. The gateway subscribes to the nodes while at least one client is
// marked, and unsubscribes when the last one clears or disconnects. Its own
// report needs no subscription and is always published.
message TransportCommand {
  bool subscribe = 1;
}

// Gateway to client: one node's view of the transport, the last report it
// sent (pages merged) plus what the gateway derives from it over a sliding
// window. Published on every complete report; the gateway's own every
// second. A client that connects gets every one held.
message NodeTransport {
  uint32 node = 1;
  TransportReport report = 2;       // counters and links as reported; peers is empty here, the edges carry them
  uint32 window_ms = 3;             // span of the rates below, 0 while one report only
  repeated TransportLinkRate links = 4;   // one per report.links entry
  repeated TransportEdge edges = 5;       // one per peer the node holds, 32 at most
  TransportWindow window = 6;       // this node's own counters over the window
}

// One link of a node over the window.
message TransportLinkRate {
  float frames_in_per_s = 1;
  float bytes_in_per_s = 2;
  float frames_out_per_s = 3;
  float bytes_out_per_s = 4;
  uint32 refused = 5;    // over the window
  uint32 rx_errors = 6;  // over the window
}

// One directed edge: what the reporting node (the observer) counts of one
// peer, cumulative as reported and as rates over the window.
message TransportEdge {
  uint32 peer = 1;
  uint32 link = 2;             // index into NodeTransport.report.links
  uint32 hops = 3;
  uint32 age_ms = 4;
  uint32 received = 5;
  uint32 lost = 6;
  uint32 duplicates = 7;
  float rx_per_s = 8;          // frames per second over the window
  float loss = 9;              // lost / (received + lost) over the window, 0..1
  float duplicates_per_s = 10;
}

// A node's own counters over the window: what happened lately rather than
// since boot.
message TransportWindow {
  uint32 sent = 1;
  uint32 refused = 2;
  uint32 dropped = 3;
  uint32 relayed = 4;
  uint32 expired = 5;
  uint32 restarted = 6;
  uint32 requests = 7;
  uint32 resent = 8;
  uint32 failed = 9;
  uint32 undecodable = 10;
  uint32 unhandled = 11;
}

enum TransportVerdict {
  VERDICT_UNKNOWN = 0;   // nothing to judge yet
  VERDICT_OK = 1;
  VERDICT_DEGRADED = 2;
  VERDICT_BAD = 3;
}

// One node as the gateway judges it from every view it holds.
message TransportNodeHealth {
  uint32 node = 1;
  TransportVerdict verdict = 2;
  bool reporting = 3;        // a complete report of its own is held (the gateway itself always)
  float worst_in_loss = 4;   // worst loss over the edges this node observes
  float worst_out_loss = 5;  // worst loss over the edges other nodes observe of it
  bool fading = 6;           // some observer heard it more than 1.5 s ago
  bool asymmetric = 7;       // a reporting node lists it and it does not list that node back
  bool link_refused = 8;     // one of its links refused frames in the window
  bool link_rx_errors = 9;   // one of its links had receive errors in the window
  bool requests_failed = 10; // it gave up on requests in the window
  bool churn = 11;           // it expired or saw restart a peer in the window
}

// Gateway to client: the verdict on the whole transport, published every
// second and to a client that connects. With no client marked it is built
// from the gateway's own view alone, which nodes_reporting says.
message TransportHealth {
  TransportVerdict verdict = 1;
  uint32 nodes_known = 2;      // the node table, the gateway included
  uint32 nodes_reporting = 3;
  uint32 worst_observer = 4;   // the edge with the worst loss, 0 when none
  uint32 worst_peer = 5;
  float worst_loss = 6;
  float frames_per_s = 7;      // sum of rx_per_s over every edge held
  repeated TransportNodeHealth nodes = 8;   // one per node of the table, 33 at most
}
```

`gateway.options`:

```
mark4.NodeTransport.links           max_count:4
mark4.NodeTransport.edges           max_count:32
mark4.TransportHealth.nodes         max_count:33
```

### 6.2 `TransportGateway` (`hub/gateway_transport.hpp` / `.cpp`)

An `AbsTransportConsumerListener` like the other gateways, holding the
publisher, the consumer, the provider (for its own pages) and the
transport's node id. Hub code: `std::map`, `std::set`, `std::deque` are
fine here.

- **Marks**: `apply(const mark4_TransportCommand &, clientId, errorOut)`
  inserts or erases the client in a `std::set<std::string>`;
  `onClientClosed(clientId)` erases it; whenever the set goes from empty
  to non-empty or back, `m_consumer.setWanted(!empty)`. An `Ack` answers
  the command like every other.
- **Own report**: `tick(nowUs)` once per second (the gateway's
  `STATUS_PERIOD_MS` cadence, driven from `housekeeping()`): walks
  `m_provider.fillPage()` pages and feeds them to
  `m_consumer.accept(selfId, page, nowUs)`; the consumer's `openLocal(selfId)`
  is called once by the gateway's constructor. Then `publishHealth()`.
- **Window**: per node, a `std::deque<Sample>` of at most `WINDOW = 10`
  samples, one per `onReport()` (the counters, the links, the peers, the
  instant). Rates: newest against oldest, `window_ms` = their span, 0 with
  a single sample (rates 0, window counters 0). `onForgotten()` drops the
  deque; `setWanted(false)` (the last client left) also drops every remote
  deque, so a client that connects later sees only the gateway's own.
- **`onReport()`**: push the sample, build `NodeTransport` (section 6.3),
  broadcast.
- **`onClientConnected()`**: every `NodeTransport` held (nodes with a
  report), then the last `TransportHealth`, to that client alone.
- **`publishHealth()`**: builds and broadcasts `TransportHealth` (section
  6.4).

### 6.3 Derivation of `NodeTransport`

For a node with samples `newest` and `oldest` (the same sample when only
one), `span = newest.t - oldest.t` in seconds:

- `report`: `newest.report` with `peers_count = 0`.
- `links[i]`: `frames_in_per_s = (newest.links[i].frames_in -
  oldest.links[i].frames_in) / span` and the three others alike; `refused`
  and `rx_errors` as deltas. All zero when `span == 0`. A link index the
  oldest sample does not have is treated as zeros.
- `edges`: one per peer of `newest`; the cumulative fields as reported;
  the deltas against the same peer id in `oldest` (a peer absent from
  `oldest` counts as zeros): `rx_per_s = d_received / span`, `loss =
  d_lost / (d_received + d_lost)` (0 when the denominator is 0),
  `duplicates_per_s = d_duplicates / span`.
- `window`: the deltas of the node counters.

### 6.4 Derivation of `TransportHealth`

Thresholds, constants of `gateway_transport.hpp`:

| constant | value | meaning |
|---|---|---|
| `LOSS_DEGRADED` | 0.01 | edge loss above this is DEGRADED |
| `LOSS_BAD` | 0.10 | edge loss above this is BAD |
| `FADING_MS` | 1500 | an edge older than this is fading |

Per node of the node table (the gateway first, then the transport table
like `nodesMessage()`):

- `reporting`: the consumer holds a complete report of it.
- edges in: the node's own edges (when reporting); `worst_in_loss` = max
  loss over them.
- edges out: every edge of every reporting node whose `peer` is this node;
  `worst_out_loss` = max loss over them; `fading` = any of them with
  `age_ms > FADING_MS`.
- `asymmetric`: this node is reporting, some other reporting node A lists
  it as a peer, and this node's edges do not list A.
- `link_refused`, `link_rx_errors`: any link of its `NodeTransport` with a
  non-zero window delta.
- `requests_failed`: `window.failed > 0`.
- `churn`: `window.expired + window.restarted > 0`.
- verdict: start OK; `max(worst_in_loss, worst_out_loss) > LOSS_DEGRADED`
  or `fading` or `link_refused` or `link_rx_errors` or `churn` makes it
  DEGRADED; `> LOSS_BAD` or `asymmetric` or `requests_failed` makes it
  BAD. A node that is not reporting and that no edge observes (no report
  anywhere lists it) is UNKNOWN.

System: `verdict` = the worst node verdict (UNKNOWN when the table holds
only the gateway); `worst_observer` / `worst_peer` / `worst_loss` = the
edge with the highest loss over every edge held (0 / 0 / 0 when none);
`frames_per_s` = the sum of `rx_per_s`; `nodes_known` = the table size,
`nodes_reporting` = the count of reporting nodes.

The derivation is written as free functions in `hub/transport_health.hpp`
(pure: inputs are the samples, output the messages), so the gateway is
wiring and the arithmetic reads alone.

### 6.5 `HubApp` wiring

- Members, in declaration order after the existing consumers: `TransportConsumer<Transport::MAX_NODES> m_transport{m_messenger, m_directory};`
  (name it `m_transportViews` to avoid clashing with `m_transport` the
  Transport), then after `m_logProvider`: `TransportProvider
  m_transportProvider{m_messenger, m_transport};`, then with the other
  gateways: `TransportGateway m_transportGateway{*this, m_transportViews, m_transportProvider, m_transport.nodeId()};`.
- `applyClientMessage`: `transport_command_tag` routes to
  `m_transportGateway.apply(...)`.
- `snapshot()`: `m_transportGateway.onClientConnected(clientId)` with the
  others.
- `housekeeping()`: `m_transportGateway.onClientClosed()` in the drainClosed
  loop; `m_transportGateway.tick(nowUs)` inside the once-per-second block,
  before `statusMessage()`.
- `gateway_codec.cpp` `fillNode()` stops writing the three removed fields.
- `GatewayStatus` is unchanged.

`hub/README.md` gains the concept (consumer, gateway, marks, own report,
the two messages) and `protocol/README.md` the gateway messages. The
`CLAUDE.md` architecture paragraphs (the gateway message list under
`protocol/`, the `transport/` paragraph) name the new messages and the
provider/consumer in one sentence each.

## 7. Clients

### 7.1 Pages (`software/hub/pages/`)

`NodeView` in `src/shared/nodes.ts` loses `received` and `lost`; the tests
that build a `Node` drop the fields.

New page `transport.html` and `src/transport/` (bundled like the others by
`esbuild.js`, one `main.ts`), on the shared `Shell` (nav, connection dot,
toasts). Dependencies added to `package.json`: `@vscode-elements/elements`
2.5.1 and `@vscode/codicons` 0.0.46 (exact versions, the lockfile
updated). `esbuild.js` gets `loader: { ".ttf": "file" }`; `src/shared/style.css`
imports `@vscode/codicons/dist/codicon.css` so every page has the icon
font. The page's own stylesheet `src/transport/transport.css` is imported
from `main.ts` (esbuild emits `dist/transport.css`, which `transport.html`
links after `style.css`).

**Theme**. VS Code injects its theme as `--vscode-*` custom properties on
the `<html>` element of the webview document and a `vscode-dark` /
`vscode-light` / `vscode-high-contrast` / `vscode-high-contrast-light`
class on its `<body>`; an iframe on another origin gets neither. The
webview host (section 7.2) posts them to the iframe as

```ts
{ type: "mark4-theme", kind: "vscode-dark" | "vscode-light" | "vscode-high-contrast" | "vscode-high-contrast-light", vars: Record<string, string> }
```

where `vars` maps every `--vscode-*` property name to its value.
`src/shared/theme.ts` listens for that message, writes each var on
`document.documentElement.style`, and puts the kind on `<body>` as its
class. Every page installs it (one call in `Shell`'s constructor), the
transport page is the one that uses it. `transport.css` defines, on
`:root`, a dark default for every `--vscode-*` variable the page and the
components it uses read, so a browser tab with no host renders; and remaps
the shared tokens (`--bg`, `--bg-panel`, `--bg-raised`, `--border`,
`--fg`, `--fg-dim`, `--accent`, `--ok`, `--warn`, `--bad`) to vscode
variables so the shell's nav follows the theme too. `@vscode-elements`
components read the same variables; the implementer checks whether they
carry fallbacks of their own for a page with no host and, if they render
unstyled without one, stops and reports.

**Subscription**. On every socket open the page sends `TransportCommand {
subscribe: true }`; on `pagehide` it sends `false`. A `vscode-checkbox`
"live reports" in the toolbar, checked by default, sends the same
command when toggled.

**Layout**: `vscode-split-layout` (horizontal, graph left 65 %, panel
right) under a banner.

**Banner**: a `vscode-badge` with the system verdict (OK green / DEGRADED
yellow / BAD red / UNKNOWN neutral, the colors from `--vscode-charts-*`
and `--vscode-badge-*`), "n reporting / m nodes", the worst edge as
"observer -> peer loss x %", the frames per second; below it the findings
as a `vscode-table` (columns: severity badge, where, what), sorted worst
first, built in TypeScript from `TransportHealth` and the `NodeTransport`s:
one line per edge above `LOSS_DEGRADED` ("A -> B loses 12 % (n/s)"), per
fading edge, per asymmetric node ("B hears A, A does not hear B"), per link
with refusals or receive errors ("board uart: 14 frames refused, ring
full"), per node with failed requests, per node with churn, and one per
node not reporting ("plant does not report: inbound only"). The thresholds
are repeated in `src/transport/health.ts` as constants with a comment
naming their C++ twin.

**Graph** (`src/transport/graph.ts`, `layout.ts`): HTML cards absolutely
positioned over one SVG layer that draws the buses, the stubs and the
edges, in a scrollable container.

Topology from the reports: for every reporting node R and every link index
L of R, a medium instance = {R} plus every peer of R with `link == L` and
`hops == 0`, of kind `links[L].kind`. Instances of the same kind sharing a
member merge. A node in no instance is placed by the hops the gateway sees
it at.

Columns, left to right, cards stacked within a column sorted by kind
order (gateway, relay, firmware, drone_sim, plant, phone, other) then id:

1. nodes that are only in UART media, one group per medium;
2. one vertical bus line per UART medium;
3. nodes in both a UART and a UDP medium (the relay);
4. one vertical bus line per UDP medium;
5. nodes that are only in UDP media;
6. nodes in no medium, labelled "via n hops" (or "unplaced" when the
   gateway does not hear them either).

Fixed geometry: column width 240 px, card height 96 px, row gap 16 px.
Each card: kind icon (`<vscode-icon>` with the same codicon names as the
extension's `KINDS`), name, id in 8 hex digits, a verdict dot, and one
port chip per link ("uart 12/15 fps" as in / out frames per second) or
"no report" for a non-reporting node. A card's ports connect to their bus
with a horizontal stub.

Edges (observer to peer): a line from the observer card to the peer card
with an arrowhead at the peer, colored by loss class (green under
`LOSS_DEGRADED`, yellow under `LOSS_BAD`, red above, the description
foreground when the window is 0) and a dashed stroke when the observer is
the only one of the pair reporting. Drawn always for an edge that is
DEGRADED or worse, and for every edge from or to the hovered or selected
card; `<title>` on the line with "observer -> peer: n fps, loss x %, dup
d/s, hops h, age a ms". A card is selected by click.

**Panel**: with no selection, "select a node". With one: header (icon,
name, id, kind, verdict badge), then `vscode-tabs`:

- Links: `vscode-table` with index, kind, frames in/s, bytes in/s, frames
  out/s, bytes out/s, refused (window), rx errors (window), and for a UART
  link the utilization "x %" of `UART_BYTES_PER_S = 92160` (921600 baud,
  10 bits per byte) computed on `bytes_in_per_s + bytes_out_per_s`.
- Peers: `vscode-table` with peer name and id, link, hops, rx/s, loss %,
  dup/s, age ms, one row per edge.
- Messenger: `vscode-table` with the report's messenger and transport
  counters, cumulative and over the window.
- a uPlot sparkline under the tabs (frames in per second summed over the
  links, and worst in-loss %, over the last 60 samples kept client side
  from successive `NodeTransport`).

For a non-reporting node the panel shows the edges other nodes observe of
it (Peers tab titled "seen by") and nothing else.

`pages/README.md` documents the page, the theme message and the
subscription.

### 7.2 Extension (`tools/vscode-mark4/`)

- `webviews.ts`: `PAGES` gains `transport: { title: "mark4 transport",
  path: "transport.html" }`; the host HTML posts the theme message of 7.1
  to the iframe on the iframe's `load` event and from a `MutationObserver`
  on `<html>` (attribute `style`) and `<body>` (attribute `class`), every
  time reading the inline `--vscode-*` properties of
  `document.documentElement.style` and the `vscode-*` class of `<body>`.
- `bench.ts`: a third line "transport page" (icon `type-hierarchy-sub`),
  command `mark4.openTransport` registered in `extension.ts` and declared
  in `package.json` next to the two others.
- `gateway.ts`: handles `transportHealth` and hands it to the nodes
  provider.
- `model.ts`: `NodeRow` gains `verdict` (0..3); `nodeRows()` takes the
  last `TransportHealth` (or undefined) and reads each node's verdict and
  flags: the tooltip line "received n, lost m, duplicates d" becomes
  "transport: ok | degraded | bad | unknown, in loss x %, out loss y %"
  plus one word per raised flag; `sameRow()` compares the verdict.
- `nodesTree.ts`: `setHealth(health)` stores it and rebuilds; the item
  icon color becomes `testing.iconPassed` for OK live, `charts.yellow`
  for DEGRADED, `charts.red` for BAD, `descriptionForeground` for
  fading or UNKNOWN; the description suffix says " degraded" / " bad"
  after the kind and id (before " fading" when both).
- Tests in `test/model.test.ts` adapt to the removed fields and the new
  parameter; no new test.
- `README.md` of the extension: the Nodes paragraph names the verdict and
  the Bench paragraph the third page.

## 8. Candidates for the testing design

Not written now (see `tests-are-a-subject-of-their-own`): the per-link
counters and the churn counters of the transport; the serial parser's
error count; the provider's paging (0 peers, 4, 5, 32) and its nothing-
without-subscriber rule; the consumer's page merge (a lost page, a cursor
out of order, `setWanted(false)` forgetting the reports); the health
derivation (rates over a window, asymmetry, the verdict ladder); the
layout's medium merge; the theme relay.

## 9. As implemented

Filled during the implementation where the code departs from the sections
above.

Step 1 (transport, wire, provider, consumer, compositions):

- `Entry::staging_valid` of 5.2 is `stagingValid`: the member naming rule
  of `.clang-tidy` (camelBack) applies to every field of the struct.
- `UdpLink::ReadOne` stays a static method and takes the error counter by
  reference (the option 3.1 left open; the smaller change, and the
  static-method naming rule stays satisfied).
- `drone_sim` ticks its provider in the main loop of `run()`, after the
  updater's `consumed()` check: 5.3 named "the updater's tick", which
  lives in the parked update loop, not in `run()`. Consequence: like its
  status stream, `drone_sim` does not report during an update session.
- A second `AbsLink` fake exists in `software/tests/unit/test_transport.cpp`
  (`FakeLink`), adapted like `RecordingLink`; 3.1 named one fake only.
- The compile-time guard of `messaging/messenger.hpp` on the highest tag
  of the schema names `transport_report` now.
- `TransportProvider` pins `PEERS_PER_PAGE` and its link bound to the
  generated array sizes with two `static_assert`s.
- Measured after the change: `mark4_TransportReport_size` 438,
  `mark4_Envelope_size` 449 (under `MAX_PAYLOAD` 512),
  `sizeof(mark4_Envelope)` 384 (the `static_assert` budget is 400).

Step 2 (gateway wire, hub, clients trimmed):

- The thresholds of 6.4 live in `hub/transport_health.hpp` rather than in
  `hub/gateway_transport.hpp`: the gateway includes the health header (for
  the sample type the free functions take), so the constants have to sit on
  that side of the include or the derivation could not read them.
- The window sample of 6.2 is `TransportSample`, declared in
  `hub/transport_health.hpp` next to the functions that read it: the
  project has one namespace, where a bare `Sample` says nothing, and the
  derivation must name the type without depending on the gateway.
- `TransportHealth` walks the node table as the gateway's own view holds
  it: its edges are the transport's node table, in the same order, so 6.5's
  constructor (which hands the gateway a node id and not the transport) is
  enough for the table 6.4 asks for.
- The hub ticks its own `TransportProvider` in `housekeeping()` (every
  loop, the provider paces itself), so it streams its report to another
  gateway that subscribes, as principle 2 says; 6.5 had listed only the
  gateway's `tick()`.

Step 3 (the pages):

- `@vscode/codicons` 0.0.46 of 7.1 does not exist: upstream publishes its
  releases under a prerelease shape and the registry holds `0.0.46-1` to
  `0.0.46-40` with no plain `0.0.46` (the last plain release is 0.0.45).
  Pinned at `0.0.46-24`, which is the `latest` tag.
  `@vscode-elements/elements` is 2.5.1 as written.
- The codicon stylesheet is a bundle entry point of its own
  (`dist/codicon.css`, esbuild) linked by `transport.html` as `<link
  id="vscode-codicon-stylesheet">`, rather than the `@import` in
  `src/shared/style.css` of 7.1: `vscode-icon` looks that id up in the
  document and loads the stylesheet it names into its own shadow root,
  where a stylesheet the document imports reaches nothing, so an import
  would have left every icon blank. Only the page with icons links it.
- The sparkline of 7.1 is two charts stacked rather than one with both
  measures: frames per second and a loss percentage share no scale, and two
  scales on one plot is a chart that cannot be read.
- 7.1 calls the split layout horizontal; the component calls a side-by-side
  divider `split="vertical"`. The geometry is the one 7.1 describes, graph
  left at 65 %, panel right.
- A column of the layout that nothing falls into takes no width, so a bench
  with no serial link does not start three columns to the right. The card
  is 200 px wide inside its 240 px column, the rest of the column being
  where the stubs run.
- A medium member that does not report is tied to its bus by a stub like
  the others, although it has no port chip to leave from: 7.1 described the
  stubs from the ports alone, which would have left those cards floating.
- The check 7.1 asked for: the components do render in a browser tab with
  no host. 211 of the 215 `--vscode-*` reads of the bundle carry their own
  dark default; the three that do not (`--vscode-checkbox-border`,
  `--vscode-checkbox-foreground`, `--vscode-font-size`) are covered by the
  `:root` block of `transport.css`.
- The hub's MIME table gained `.ttf` (`font/ttf`) for the codicon font
  the page links.

Step 5 (streams, C++ and GDScript):

- 10.1 states rules, not methods. Both ports gained two helpers so the two
  sides of the rule read once: `Transport::track()` / `_track()`, which take
  one frame into the stream its destination names, and
  `Transport::nextSeq()` / `_next_seq()`, which take the next sequence of
  one stream.
- A unicast to a destination the transport does not know consumes no
  sequence any more: the entry has to be found before the header is
  written, so the refusal happens first. It used to consume one of the
  single counter. The counters `send()` and `sendKeepalive()` move
  (`dropped`, `refused`) are unchanged.
- A consequence of 10.2 worth knowing before 10.5 and 10.6 are written:
  `unicast_heard` is true for nearly every direct peer, because the
  transport's own greeting to a newcomer (the unicast keepalive of the
  Presence rule) is a frame of that stream. An edge reads as keepalive only
  while the peer has sent that greeting and nothing since, which is the
  case of a peer that learnt this node and never addressed it again.
- Nothing else departs. `test_transport.cpp` needed one line
  (`node.lastSeq` became `node.unicastSeq`); every other assertion on
  `lost`, `duplicates` and `received` holds unchanged under the new rule,
  and `sim-godot/tests/transport_check.gd` and `test_plant_link.cpp`
  asserted on nothing the rule moves.

Step 5 (streams, Dart):

- `_emit()` numbers the frame after the destination is resolved, so a
  unicast to a node the table does not hold is refused without consuming a
  sequence; the old code took the node's single counter before the lookup.
  The unicast sequence lives on the destination entry, which leaves no
  other order.
- The stream accounting of `_learn()` is a private `_account()`, with
  `_takeSeq()` next to it, which `_onBootId()` calls too: Dart rebuilds a
  reincarnated entry field by field where the C++ assigns a fresh `Node{}`,
  so the reset of `txSeq` and of both streams is written out.
- `frame.dart`: the doc of `FrameHeader.seq` read "per-sender counter",
  the words of `transport/frame.hpp`, and now names the stream. 10.4 named
  `transport_node.dart` alone, but the line had become false.
- `docs/mobile-app.md` describes the same node ("last sequence", the
  `(src, seq)` duplicate drop) and says one sequence per stream and
  `(src, dst, seq)` now, for the same reason.
- No test was adapted: nothing under `software/mobile/test/` exercises the
  real `TransportNode` (the suite runs the managers over
  `FakeTransportNode`), so no test asserted on the old rule.

Step 6 (hub and page counts):

- The tooltip carries both halves: 10.6 appends `keepalive only` to the
  edge tooltip and its amendment gives the words and the condition, so a
  quiet edge reads `loss 0 % (0/10 over 10 s), ..., keepalive only (10
  frames over 10 s)`. The loss segment is printed for every edge, quiet or
  not; only the judgement drops it.
- `lossClass()` takes the edge rather than a loss: the two counts it now
  needs live on the edge and both call sites hold one. Its `idle` class (no
  window at all) is still decided first, so an edge of a node that has sent
  one report only keeps the class it had.
- An edge with no window at all is quiet to the words (the tooltip's note,
  the Peers table's stream column): no frame is fewer than
  `LOSS_MIN_FRAMES`. Only the class tells `idle` from `quiet`, and both are
  drawn in the same color, so what is seen is one thing.
- `.tp-edge.quiet` is written after `.tp-edge.one-sided` in `transport.css`:
  the two selectors have the same specificity, so the order is what decides
  an edge that is both, and it is drawn dotted.
- The Peers table loses its `dup/s` column: 10.6 lists `dup` among the three
  cumulative counters and leaves the window's rate no column. Its eleven
  cells are built by one helper for both shapes of the table (a node's own
  peers, and the observers of a node that does not report).
- `TransportHealth.frames_per_s` still sums the `rx_per_s` of every edge,
  the quiet ones included: 10.5 takes a quiet edge out of the loss verdict
  alone.
- `pages/README.md` said "one line per edge that loses frames or has gone
  quiet" of a fading edge; the word now names something else, so that line
  reads "fades".
- The editor extension reads no edge (it takes `TransportHealth` alone), so
  the three new fields cost it nothing.

## 10. Sequence per stream, and what a percentage is worth

Decided on 2026-09-19, after the first bench run of the page: with a hub,
three `drone_sim` and the plant, every edge read 58 % to 99 % loss while
nothing was lost. The transport numbers every frame a node sends with one
counter, whatever the destination; a receiver only sees the frames for it
and the broadcasts, and counts every gap as a loss. Every unicast to
someone else therefore reads as a lost frame: a `drone_sim` hears from
another `drone_sim` its keepalives alone (one per second) and "loses" the
hundreds of `SimActuator` per second that went to the plant. `NodeTable`
carried the same numbers before this feature; the page is what made them
readable. This section fixes the accounting, then what the page shows next
to a percentage so it can be read.

### 10.1 Streams (`transport/`, C++)

A sequence numbers one **stream**: the frames from one sender to one
destination, the broadcast being a stream of its own. The header does not
change; what `seq` means does.

Sender (`Transport`):

- `Node` gains `std::uint16_t txSeq = 0U;` ///< next sequence of the unicast
  stream to it. It starts at 0 when the entry is created and when the entry
  is reset by a reincarnation (`onBootId()` rebuilds it).
- `m_nextSeq` becomes `m_broadcastSeq`, the broadcast stream.
- `send()` and `sendKeepalive()`: a broadcast takes `m_broadcastSeq++`; a
  unicast takes `target->txSeq++` of its destination (the unicast
  keepalive to a newcomer included: it is the first frame of that stream,
  sequence 0).

Receiver (`learn()`):

- `Node::lastSeq` is replaced by four fields: `unicastSeq`,
  `unicastHeard` (false until the first frame of that stream), `broadcastSeq`,
  `broadcastHeard`.
- The stream of a frame is decided by its `dst`: `m_nodeId` is the unicast
  stream of `src`, `BROADCAST_NODE` its broadcast stream, anything else is
  a frame for another node that this node relays. On the first frame of a
  stream the sequence is taken silently and the flag set; then `delta ==
  0` is a duplicate (dropped, `duplicates` counted), `1 < delta <
  RESYNC_THRESHOLD` adds `delta - 1` to `lost`, a larger jump counts as
  nothing; `received` counts every accepted frame of either stream.
- A frame for another node refreshes the presence of its sender (link,
  address, `lastSeenUs`, `hops`) and nothing else: no sequence, no
  duplicate drop, no loss, no `received`. It is relayed as before. The
  broadcast stream keeps the duplicate drop that stops a triangle of
  relays from looping a broadcast (the existing test); a unicast that loops
  is bounded by `MAX_HOPS` alone. No relay triangle exists today.
- `onBootId()` on a reincarnation rebuilds the entry as today, then takes
  the keepalive's sequence into the stream its `dst` names, the other
  stream unheard.

`transport/README.md` rewrites the sequence paragraphs (Frame, API,
Relay) with this rule. The existing tests of `test_transport.cpp` that
assert on `lost`, `duplicates` or the sequence are adapted to the rule (a
test sending unicasts to two destinations from one node now expects no
loss at either); no new test.

### 10.2 Wire

`TransportPeer` gains `bool unicast_heard = 8;` "the peer has sent this
node at least one unicast since it was learnt: the edge carries more than
the keepalive". The provider copies `Node::unicastHeard`. The consumer is
unchanged.

### 10.3 The GDScript plant (`sim-godot/scripts/transport/transport.gd`)

The same rule, field for field: the node dictionary gains `tx_seq`,
`unicast_seq`, `unicast_heard`, `broadcast_seq`, `broadcast_heard` and
loses `last_seq`; `_next_seq` becomes `_broadcast_seq`; `send()` and
`_send_keepalive()` pick the stream by destination; `_on_frame()` decides
the stream by `dst` and touches presence alone for a frame addressed to
another node (the plant has one link and relays nothing, so it only
forwards nothing); `_on_boot_id()` resets the streams as 10.1 says. The
header comment of the file states the rule. `sim-godot/tests/transport_check.gd`
and `software/tests/unit/test_plant_link.cpp` are adapted only if they
assert on the old rule.

### 10.4 The Dart phone (`software/mobile/lib/back/transport/transport_node.dart`)

The same rule: `_Node.lastSeq` becomes `unicastSeq`, `unicastHeard`,
`broadcastSeq`, `broadcastHeard` plus `txSeq`; `_nextSeq` becomes
`_broadcastSeq`; `send()`, the keepalive, `_learn()` and `_onBootId()`
follow 10.1. The doc comment of the class states the rule. The fakes and
tests under `software/mobile/test/` are adapted only where they assert on
the old rule; `flutter analyze`, `dart format --set-exit-if-changed` and
`flutter test` pass.

### 10.5 The hub

`gateway.proto`, `TransportEdge` gains:

```proto
bool unicast_heard = 11;    // the edge carries more than the keepalive
uint32 window_received = 12;  // frames received over the window
uint32 window_lost = 13;      // frames lost over the window
```

`fillNodeTransport()` copies the flag and the two deltas it already
computes for `loss`.

A percentage over ten frames is not a measurement. `hub/transport_health.hpp`
gains `LOSS_MIN_FRAMES = 20`: an edge whose `window_received +
window_lost` is below it is **quiet** and is not judged on its loss (it
still counts for fading), and it is never the worst edge of
`TransportHealth`. A keepalive-only edge over a 10 s window holds 10
frames and is therefore judged on presence alone, which is what a keepalive
is for; an edge with traffic is judged on its real loss.

### 10.6 The page

Wherever a percentage is printed, the count it was computed on is printed
next to it, as `lost/total over N s` with the window's span in whole
seconds:

- edge tooltip: `A -> B: loss 3 % (1/33 over 10 s), 3.3 fps, dup d/s, hops h,
  age a ms`, plus `keepalive only` when `unicast_heard` is false;
- findings line: `loses 3 % (1/33 over 10 s)`; a quiet edge (10.5) raises
  no finding, the page mirrors `LOSS_MIN_FRAMES` in `health.ts` like the
  other thresholds, and `lossClass()` returns `quiet` for it (drawn in the
  description foreground like an edge with no window);
- banner: `worst A -> B 3 % (1/33)`;
- Peers table of the panel: peer, stream (`unicast` or `keepalive`), link,
  hops, rx/s, loss %, window (`lost/total`), received, lost, dup (the three
  cumulative), age.

A keepalive-only edge is drawn dotted (short dashes); the dashed stroke
keeps its meaning (the other direction is not reported). The README of the
pages says what a quiet edge and a keepalive-only edge are.

Amended on 2026-09-19 once 10.1 was implemented: `unicast_heard` is true
for nearly every direct peer, because the transport's greeting to a
newcomer (the unicast keepalive of the Presence rule) is a frame of that
stream, so the flag cannot tell an edge with traffic from one without.
The page therefore reads "keepalive only" from the window alone: an edge
is **keepalive only** when it is quiet (10.5, fewer than `LOSS_MIN_FRAMES`
frames over the window), which is the practical meaning of the words. The
`unicast_heard` field stays on the wire as documented in 10.2 and the
page does not show it; the Peers table's stream column reads `quiet` or
`traffic` from the same rule, and the tooltip says `keepalive only (n
frames over N s)`.
