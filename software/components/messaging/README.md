# messaging

The postman and the sender: the one place an `Envelope` of
`protocol/mark4.proto` meets the transport, in both directions. A static
library over `transport` (the frames) and `protocol` (the codec), no heap,
no iostream, no exceptions, no RTTI, and it builds for the F405 as it
stands. Inbound, every payload the transport delivers is decoded once and
handed to the one handler that claimed its body tag; outbound, `send()`
encodes one message on the stack and unicasts it once, and `request()`
numbers it, keeps it and resends it until the destination acknowledges it.
The messenger is also the transport's presence listener on this side: what
a node's arrival and departure mean reaches the handlers through it.

## The model

A handler is an object that consumes the messages of some body tags. It
names them once, as a `static constexpr` array, and hands the array to the
base constructor; attaching and detaching happen in that constructor and
destructor, so declaring a handler as a member after the messenger is the
whole wiring:

```cpp
class RcReceiver final : public AbsMessageHandler
{
  public:
    static constexpr std::array<pb_size_t, 1> TAGS{mark4_Envelope_rc_tag};

    explicit RcReceiver(Messenger &messenger)
        : AbsMessageHandler(messenger, TAGS)
    {
    }

    bool onMessage(std::uint32_t src, const mark4_Envelope &envelope, std::uint64_t nowUs) override;
};
```

The tags travel through the constructor rather than a virtual call because
the base constructor runs before the derived vtable exists; the array must
outlive the handler, which a `static constexpr` member guarantees. `src` is
where an answer goes, `nowUs` the instant of the poll that delivered the
message. A handler may `send()` from inside `onMessage()`.

## API (`messaging/messenger.hpp`)

```cpp
std::array<PendingRequest, Messenger::BOARD_PENDING_REQUESTS> pending{};  // owned by the App
Messenger messenger(transport, pending);    // value member of the App, after the Transport
RcReceiver rc(messenger);                   // handlers: members after the messenger
messenger.init();                           // false when a tag is claimed twice or out of range
messenger.setTap(tap, context);             // optional: the raw bytes of every payload, one at most
messenger.poll(nowUs);                      // once per loop: the one caller of transport.poll()
messenger.tick(nowUs);                      // resends and gives up; poll() ends with it
messenger.send(dst, envelope);              // unicast one message once; BROADCAST_NODE is refused
messenger.received() ... unmatchedAcks();   // counters
```

The pending table is the composition's: without a heap a fixed array must
know its size at compile time, and a span keeps one `Messenger` type for
every composition, the way a handler already hands the messenger a span
over its tags. `BOARD_PENDING_REQUESTS` (4) is what a board affords,
`HUB_PENDING_REQUESTS` (256) what a gateway needs, where one node
appearing costs several requests at once.

## Subscribers

`messaging/subscriber_table.hpp` is the set of node ids a stream goes to:
`SubscriberTable<N>`, a fixed array with `add()`, `remove()`,
`contains()`, `size()`, `Capacity()` and `id(index)`, no heap and no order (a removal
moves the last entry into the freed slot). A provider holds one per stream
it emits: it adds the node that subscribed, answers `false` when `add()`
finds the table full, removes the node on `onNodeDown()`, and walks the
table to emit. `N` is a constant of each stream, small because a board's
link pays every entry.

## Tables

`messaging/table_pull.hpp` is the walk of one paged table as the consumer
that asks for it keeps it: `TablePull<T, MAX>`, a fixed array with
`reset()`, `applyPage(total, cursor, items)`, `abandon()`, the reads
`complete()`, `abandoned()`, `cursor()`, `total()`, `size()`, `items()` and
`Capacity()`, and the id of the page request still outstanding
(`requestId()` / `setRequestId()`). The consumer sends the page request
itself, because it is the only one that knows the message type, and hands
the id back; `applyPage()` ignores a page whose cursor is not the one the
walk waits on (a duplicate or a stale answer), restarts the table on a page
at cursor 0, appends up to MAX and stops there, advances the cursor by the
item count and marks the walk complete when the cursor reaches the total or
the page is empty. A page request given up on reaches the consumer as
`onRequestFailed(dst, id)`, and an id equal to the pull's `requestId()`
abandons it.

## Dispatch

`poll(nowUs)` drains the transport once. For each payload delivered to this
node, in order:

1. the tap, if one is set, sees the raw bytes and `src`, before anything
   else (a gateway mirrors frames with it);
2. the bytes are decoded into one `mark4_Envelope` on the stack; what is
   not a valid Envelope stops here;
3. an `Envelope` whose body is the `ack` completes the pending request its
   `(src, request_id)` names and stops here: that tag is the messenger's
   own, and a handler claiming it is shadowed like any duplicate claim;
4. an `Envelope` carrying a non-zero `request_id` is acknowledged to `src`
   before anything else happens, whether or not a handler claims its tag: a
   node asked something it does not understand acknowledges and drops it,
   and a `Reboot` is acknowledged before the reset rather than never;
5. `which_body` indexes the table (`TAG_SLOTS` = 64 slots, every tag of the
   schema is below it, a `static_assert` says so for the highest one); an
   empty slot stops here;
6. the handler's `onMessage(src, envelope, nowUs)` runs.

The transport's keepalive is never a payload, so the messenger never sees
it. One handler per tag: a claim that finds its slot taken (or
a tag past the table) fails, the handler hears nothing on that tag and does
not take the slot over when the owner goes away; `init()` returns false for
as long as such a handler lives. Handlers are checked at `init()` like the
rest of the composition, not on every frame.

## Counters

| counter | meaning |
|---------|---------|
| `received()` | payloads the transport delivered |
| `undecodable()` | payloads that were not a valid Envelope |
| `unhandled()` | decoded messages with no handler for their tag |
| `handled()` | messages whose handler returned true |
| `ignored()` | messages whose handler returned false |
| `sent()` | frames that left on a link: one per `send()`, one per send of a request |
| `refused()` | `send()` and `request()` calls refused: a broadcast destination, a destination the transport does not know (`request()`), a full pending table, an encoding failure, a transport that took nothing |
| `requests()` | requests started |
| `resent()` | requests sent again because no acknowledgement came |
| `completed()` | requests acknowledged by their destination |
| `failed()` | requests given up on, out of sends or with the node that went down |
| `acked()` | acknowledgements this node sent, one per numbered message it received |
| `unmatchedAcks()` | acknowledgements matching no pending request |

`received()` = `undecodable()` + `unhandled()` + `handled()` + `ignored()`
+ the acknowledgements consumed.

## Requests

```cpp
struct RequestPolicy { std::uint64_t periodUs = 500'000U; std::uint8_t retries = 5U; };
// inside a handler, which is the owner of what it sends:
std::uint32_t id = request(dst, envelope, RequestPolicy{IDENTITY_TIMEOUT_US, IDENTITY_RETRIES});
void onRequestFailed(std::uint32_t dst, std::uint32_t requestId) override;  // given up on
```

`request()` returns the id it took, never 0, and 0 when it refused. Keep the
id where `onRequestFailed()` has something to decide with it (a table pull
abandons itself, a subscribe leaves its flag false) and drop it where it has
not.

A message sent with `request()` carries a `request_id` drawn from a counter
of this node's own (never 0, wrapping back to 1), so a request is the pair
(peer node id, request id). The encoded bytes are kept in the pending table
and sent again every `policy.periodUs` until `policy.retries` sends have
gone out, the first one included; then the entry is freed and its owner
hears `onRequestFailed(dst, requestId)`. An `Ack` carrying the same id from
the same node completes it at any point. A request to a node the transport
does not know is refused at once, not kept: the caller waits for
`onNodeUp()`. A first send the transport refuses (a full UART ring) is kept
all the same, because the retry is exactly what covers it.

The policy is per call, with those defaults: a reboot and a table page do
not want the same values. This is at-least-once delivery, so what travels
as a request is an idempotent state ("this is the configuration", "set this
parameter", "reboot", "here is page 3"); a message that would not survive
being applied twice does not go through `request()`. Stream data goes by
`send()`: a sample is only worth its own instant.

An acknowledgement says "arrived", and nothing about what was done. An
answer that matters is a request of its own, correlated by its content and
never by the request id it answers.

## Presence

The messenger is the transport's `AbsPresenceListener` on this side, and
relays both events to every handler exactly once, whatever number of tags
it claims:

```cpp
void onNodeUp(std::uint32_t nodeId) override;    // empty by default
void onNodeDown(std::uint32_t nodeId) override;  // empty by default
```

A node that goes down takes its pending requests with it: each is freed and
reported to its owner with `onRequestFailed()` before the handlers hear
`onNodeDown()`. The order in which the handlers are called is not
guaranteed and carries no meaning: it follows the tag table, not the attach
order.

## What it does not do

- No queue: a message is handled inside the poll that delivered it, or
  never. What must survive the poll is the handler's to copy (a pending
  request is kept as encoded bytes, not as a message).
- No ordering between requests: they are numbered, not sequenced, and a
  resend may overtake a later request.
- No retry and no acknowledgement on `send()`: it reports whether the frame
  left, and nothing else. What must arrive goes through `request()`.
- No broadcast: a broadcast destination is refused. What every node must
  hear (a `Status`, an `Announce`, a `Log` line) is not a message to one
  node and does not go through here.
- No routing, no addresses: the transport knows where a node is; the
  messenger knows which handler wants which message.
