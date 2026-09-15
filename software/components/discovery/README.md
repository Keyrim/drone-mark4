# discovery

Who is who on the wire. A static library over `messaging` (the dispatch
and the sender, which bring `transport` and `protocol` with them), no heap,
no iostream, no exceptions, no RTTI, and it builds for the F405 as it
stands. Two classes: `Discovery`, the handler every node carries, answers
an `IdentityRequest` of `protocol/mark4.proto` with the node's `Announce`;
`DiscoveryDirectory`, for a node that also needs to know who is around,
asks every node the transport hears, retries on a timeout, gives up after a
few, and keeps the answers by kind. `drone_sim` carries a `Discovery`, the
hub a `DiscoveryDirectory`; the firmware, the relay, the plant, the
campaign and the phone do not answer yet.

## Identity

Presence on the wire is the transport's keepalive, a header-only frame
that carries no identity. What a node is (its kind, its name, its MCU, the
build it runs, the schema it speaks) travels only on request: whoever
wants to know sends an `IdentityRequest`, unicast, and the node answers
with its `Announce`, unicast, to the requester. Nothing is broadcast and
nothing is sent unsolicited. A node that cannot say who it is does not
exist for the ground tools.

```cpp
class Discovery : public AbsMessageHandler
{
  public:
    static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_identity_request_tag};
    Discovery(Messenger &messenger, const mark4_Announce &self);   // self is copied
    bool onMessage(std::uint32_t src, const mark4_Envelope &envelope, std::uint64_t nowUs) override;
    const mark4_Announce &self() const;
    std::uint32_t answered() const;                                 // requests answered
};
```

Declared as a member after the messenger, it is wired: `onMessage()` sends
`self` to `src` as a request of its own (the answer that matters is
acknowledged like everything that has to arrive) and counts. The composition builds the `Announce` once,
from what it knows of itself (`drone_sim`: kind `DRONE_SIM`, mcu `SIM`,
`WIRE_HASH`, no build identity since a process is not a packaged image).

## Directory

`DiscoveryDirectory` is a `Discovery` (it answers too) that hears presence
through the messenger, like every handler. A node that appears gets an entry
`PENDING`; the next `tick()` asks it, once, with the policy
`{IDENTITY_TIMEOUT_US, IDENTITY_RETRIES}`: the resends every 500 ms and the
giving up after 5 sends are the messenger's, and when it gives up
`onRequestFailed()` moves the entry to `MUTE` and leaves it alone. An
`Announce` from the node, at any time, makes the entry `KNOWN`: the announce
is stored, the `wireMismatch` flag compares its `wire_hash` with this
build's `WIRE_HASH`, `hops` is read from the transport's node. A node the
transport expires takes its entry with it.

```cpp
struct DirectoryEntry
{
    enum class State : std::uint8_t { PENDING, KNOWN, MUTE };
    std::uint32_t id;                 // node id
    State state;                      // PENDING: asked, no answer yet; KNOWN: announce valid; MUTE: gave up
    mark4_Announce announce;          // valid when KNOWN
    bool wireMismatch;                // KNOWN and announce.wire_hash != WIRE_HASH
    std::uint8_t hops;                // relays between this node and it
    std::uint64_t askedUs;            // instant the request was started
    std::uint8_t requests;            // requests started, 0 or 1: the resends are the messenger's
    std::uint64_t updatedUs;          // instant of the last state change
};

DiscoveryDirectory directory(messenger, transport, self);   // after the messenger, whose presence events it takes
directory.init();                                           // false past MAX_LISTENERS listeners
directory.tick(nowUs);                                      // once per loop, after messenger.poll()
directory.find(id); directory.size(); directory.entry(i);   // the table, a dense prefix in no particular order
directory.nodesOfKind(kinds, out);                          // the KNOWN entries of those kinds, copied into out
directory.requests(); directory.learnt(); directory.muted(); directory.dropped();
```

The directory never reads a clock. `onNodeUp()` has no instant and sends
nothing: it creates the entry with no request behind it, and `tick()` asks
at once. A request is counted whether or not the messenger took it: a full
pending table refuses it, and the next tick asks again. `MAX_ENTRIES` is the transport's `MAX_NODES`; a node that
appears while the table is full is counted in `dropped()` and gets its
entry from its `Announce` if one ever arrives (a node the transport learnt
before the directory existed is handled the same way).

## Listeners

```cpp
class AbsDirectoryListener
{
  public:
    explicit AbsDirectoryListener(DiscoveryDirectory &directory);   // attaches
    virtual ~AbsDirectoryListener();                                // detaches
    virtual void onIdentity(const DirectoryEntry &entry) = 0;      // KNOWN now, or the announce changed
    virtual void onForgotten(std::uint32_t nodeId) = 0;            // the transport expired the node
};
```

Same shape as `AbsPresenceListener`: a fixed table of `MAX_LISTENERS`
(8), attached in the constructor and detached in the destructor, `init()`
false for as long as one past the table lives. `onIdentity()` fires when
an entry becomes `KNOWN` and again when a later `Announce` differs from
the stored one (field by field; a node that rebooted on another build);
an identical answer is silent. The notifications run from inside
`onMessage()` and `onNodeDown()`, so a listener may `send()` from them,
as it may from a presence callback.

## Counters

| counter | meaning |
|---------|---------|
| `answered()` | `IdentityRequest`s answered with `self` |
| `requests()` | `IdentityRequest`s started, the refused ones included; what the messenger resent is its own counter |
| `learnt()` | `Announce`s stored as an identity |
| `muted()` | nodes whose request ran out of sends |
| `dropped()` | nodes that appeared while the table was full |

## What it does not do

- No broadcast: an identity is a conversation with one node.
- No clock: `tick()` and the messenger's poll bring every instant.
- No retry of its own: the timeout and the count are a `RequestPolicy` the
  messenger applies, and giving up reaches the directory as
  `onRequestFailed()`.
- No interpretation of the identity: what a kind means (a drone to pull
  telemetry from, a relay to update) is the composition's business.
