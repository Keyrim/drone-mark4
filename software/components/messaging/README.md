# messaging

The postman and the sender: the one place an `Envelope` of
`protocol/mark4.proto` meets the transport, in both directions. A static
library over `transport` (the frames) and `protocol` (the codec), no heap,
no iostream, no exceptions, no RTTI, and it builds for the F405 as it
stands. Inbound, every payload the transport delivers is decoded once and
handed to the one handler that claimed its body tag; outbound, `send()`
encodes one message on the stack and unicasts it. No composition uses it
yet.

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
Messenger messenger(transport);             // value member of the App, after the Transport
RcReceiver rc(messenger);                   // handlers: members after the messenger
messenger.init();                           // false when a tag is claimed twice or out of range
messenger.setTap(tap, context);             // optional: the raw bytes of every payload, one at most
messenger.poll(nowUs);                      // once per loop: the one caller of transport.poll()
messenger.send(dst, envelope);              // unicast one message; BROADCAST_NODE is refused
messenger.received() ... refused();         // counters
```

## Dispatch

`poll(nowUs)` drains the transport once. For each payload delivered to this
node, in order:

1. the tap, if one is set, sees the raw bytes and `src`, before anything
   else (a gateway mirrors frames with it);
2. the bytes are decoded into one `mark4_Envelope` on the stack; what is
   not a valid Envelope stops here;
3. `which_body` indexes the table (`TAG_SLOTS` = 64 slots, every tag of the
   schema is below it, a `static_assert` says so for the highest one); an
   empty slot stops here;
4. the handler's `onMessage(src, envelope, nowUs)` runs.

The transport's header-only keepalive is never a payload, so the messenger
never sees it. One handler per tag: a claim that finds its slot taken (or
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
| `sent()` | `send()` calls whose frame left on a link |
| `refused()` | `send()` calls refused: a broadcast destination, an encoding failure, a transport that took nothing |

`received()` = `undecodable()` + `unhandled()` + `handled()` + `ignored()`.

## What it does not do

- No queue: a message is handled inside the poll that delivered it, or
  never. What must survive the poll is the handler's to copy.
- No retry, no acknowledgement: `send()` reports whether the frame left,
  and nothing else.
- No broadcast: a broadcast destination is refused. What every node must
  hear (a `Status`, an `Announce`, a `Log` line) is not a message to one
  node and does not go through here.
- No routing, no addresses: the transport knows where a node is; the
  messenger knows which handler wants which message.
