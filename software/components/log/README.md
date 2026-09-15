# log

The one logging library of every node: the board, `drone_sim`, the hub, the
ESP32 relay.
A source file declares its module once, logs printf-style through it, and
every line carries the module, a level and a timestamp to up to two sinks:
a console (stdout on a desktop, RTT on the board) and the provider, which
sends it as a `mark4.Log` message to the nodes that subscribed to this
node's lines. Levels are per module and set at runtime, from the process or
from the wire.

```cpp
#include "log/module.hpp"
#include "log/module_ids.hpp"

namespace
{
    mark4::LogModule MODULE{mark4::LOG_MODULE_PLATFORM_IMU, "platform/imu"};
}

MODULE.info("found at 0x%02X", address);   // also trace / debug / warn / error
```

Four CMake targets. `log` is a leaf (it links `drone_warnings` alone, no
heap, no iostream, no `std::function`, plain C++17) and builds on every
preset; on desktop it also holds `ConsoleSinkPosix`. `log_wire` adds the
level codec and the two helpers that fill the messages of the schema
(`log/wire.hpp`: `logLevelToWire()`, `logLevelFromWire()`,
`logFillModuleInfo()`, `logFillModulesPage()`) and links `protocol/`.
`log_provider` adds `LogProvider` (`log/provider.hpp`), the library on the
messenger, and links `messaging`; `log_consumer` adds `LogConsumer<N>`
(`log/consumer.hpp`), what a ground node keeps of another node's log. The
RTT sink lives next to the RTT driver, `platform_stm32/rtt_sink.hpp`.

## Modules

`LogModule(id, name)` links itself into a process-wide intrusive list at
construction: a `static` object per source file is the whole registration,
there is no table to maintain and nothing allocates. Several modules in one
file are fine when the file speaks for several areas (`drone_sim_app.cpp`
has `app/boot`, `sim/link`, `flight/core`).

Names are hierarchical with `/`, at most 32 characters, so a client can
address an area by prefix: `platform/imu`, `platform/baro`, `ota/store`,
`ota/updater`, `transport/uart`, `sim/link`, `flight/core`, `gateway/core`,
`gateway/ws`, `app/boot`, `app/main`, `app/status`, `rc`, `log/core` (the
library itself).

Ids are `uint16_t`, unique per node, chosen at compile time:

- shared code (a driver, a store: any file linked by more than one node)
  takes its id from `log/module_ids.hpp`, so the same file has the same id
  and name on every node;
- an application's own files take theirs from its `log_modules.hpp`,
  starting at `LOG_MODULE_APP_BASE` (256).

Two modules with the same id on one node is a mistake the wire cannot see
(a `set` would move the first one found); keep the tables short and
adjacent.

## Levels

`TRACE < DEBUG < INFO < WARN < ERROR`, same values on the wire
(`mark4.LogLevel`, `TRACE = 0 .. ERROR = 4`; nothing persists a level, so
the enum was renumbered rather than appended to). Every module starts at
`INFO`. The check against the module's level happens before any argument
is formatted; the text is then `vsnprintf`ed into a 96-byte buffer
(`LogModule::MAX_TEXT`), truncated, never allocated. The five methods carry
`__attribute__((format(printf)))`, so `-Wformat` catches a mismatched
argument like it does for `printf`.

Runtime control, from the process:

```cpp
logSetLevel(LOG_MODULE_PLATFORM_IMU, LogLevel::DEBUG);  // one module, by id
logSetLevelByPrefix("platform/", LogLevel::DEBUG);      // every module under it
for (LogModule *m = logModules(); m != nullptr; m = m->next()) { ... }
```

## Sinks and clock

`AbsLogSink::write(const LogRecord &)` receives `{moduleId, moduleName,
level, timestampUs, text}`; the pointers are valid for the call only. At
most `LOG_MAX_SINKS` = 2 are registered (`logAddSink()` / `logRemoveSink()`).
A sink filters nothing: the module's level already did.

- `ConsoleSinkPosix` (desktop, and the ESP32 relay: its console is plain
  stdio too, so the same sink serves it): `HH:MM:SS.mmm LEVL module: text`
  on stdout, flushed per line. On a board the clock is its uptime.
- `RttSink` (board): `t_ms LEVL module: text` on the RTT up buffer.
- `LogProvider` (any node, `log_provider`): encodes a `Log` message and
  sends it to every node that subscribed to this node's lines. Rate limited
  to `MAX_LINES_PER_SECOND` = 50; the lines refused are counted and the
  count goes out once per second as a WARN of `log/core`, written straight
  to the subscribers so it cannot be rate limited itself.

The timestamp comes from a clock function pointer registered once
(`logSetClock(fn, context)`): the library never reads a clock, like
flight-core and transport. Until it is registered, records carry 0 (the
board's first lines, before its timer runs).

The library is not thread-safe. Every node of the project logs from its
one loop thread; the hub's websocket threads log nothing.

## Wire

`LogProvider` (`log/provider.hpp`, target `log_provider`) is both a sink of
the library and an `AbsMessageHandler` of the composition's `Messenger`:
declaring it as a member after the messenger and handing it to
`logAddSink()` is the whole wiring. It is the one route from this node's
log to the wire, and it never broadcasts: a line leaves because a consumer
asked for it.

Four messages of `mark4.proto`, one `Envelope` body each:

- `LogSubscribe { enabled }`, consumer to node: take this node's line
  stream, or stop it. Answered by `LogSubscription { enabled }`, the
  subscription the node holds afterwards; `enabled` comes back false when
  the table is full (`MAX_SUBSCRIBERS` = 2). A node that goes down is
  dropped from the table. The request and its answer are two message types
  on purpose: the gateway is both a log provider and a log consumer, and a
  messenger holds one handler per body tag, so the provider claims
  `LogSubscribe` and the consumer `LogSubscription`.
- `Log { timestamp_us, level, module_id, text (96) }`, one per line let
  through, to each subscriber. The module is named by id: the table below
  gives the name.
- `LogModulesRequest { cursor }` in, `LogModules { total, cursor, modules
  (8 at most) }` out: one page per request, because 48 names of 32
  characters do not fit one frame. The last page is the one where
  `cursor + modules_count == total`. The consumer paces the walk.
- `LogSetLevel { module_id, level }` in, the module's `LogModuleInfo { id,
  name (32), level }` out to the requester, and the same to every other
  subscriber when the level really moved: what a stream carries changes for
  everyone holding it. A module this node does not have is refused with a
  WARN and nothing is answered; a level this build does not know leaves the
  module where it is and the answer says so.

Prefix semantics live in the client: it knows the names from the table and
sends one `LogSetLevel` per matching id. The node stays dumb: no string
matching on the wire, no dynamic registration.

A node that consumes its own lines cannot receive them on the wire, since a
messenger refuses its own node as a destination. It registers an
`AbsLogSink` with `LogProvider::setLocalSink()` instead, fed after the rate
limit exactly like a subscriber: that is how the gateway's own lines reach
its clients.

## The consumer

`log/consumer.hpp` holds the other side of the same concept, for a node
that reads another node's log: `LogConsumer<N>` (target `log_consumer`), an
`AbsMessageHandler` and an `AbsDirectoryListener` at once, sized by the
composition. Header-only, fixed tables, no heap.

```cpp
LogConsumer<Transport::MAX_NODES> logs{messenger, directory};  // after both
logs.setLevel(node, moduleId, level);   // also refresh()
```

The kinds that carry a `LogProvider` are its own constant (`KINDS`:
`FIRMWARE`, `DRONE_SIM`, `RELAY`, `GATEWAY`), so the composition names no
kind. A node of one of those kinds whose announce matches this wire hash is
opened from `onIdentity()`: one `LogSubscribe { enabled: true }` and one
table walk, a `TablePull<mark4_LogModuleInfo, MAX_MODULES>` asking one
`LogModulesRequest` per page. The listeners hear `onModules()` once the
table is whole, and again when a `LogModuleInfo` moves one module's level;
every line reaches `onLine()`. A node that goes down loses its entry and
the listeners hear `onForgotten()`.

The gateway holds one and exposes what it keeps as `Node.log_modules` in
the `NodeTable`, so a client connecting late knows every module and level
without asking. The pages toast WARN and ERROR lines only, prefixed with
the module name resolved from that table (`#id` when the table has not
arrived).

## Setting a level from a client (TypeScript, the generated codec)

```ts
// modules = node.logModules from the NodeTable; prefix "platform/"
for (const module of modules.filter((m) => m.name.startsWith("platform/"))) {
    socket.send(frameMessage(nodeId, create(EnvelopeSchema, {
        body: { case: "logSetLevel", value: { moduleId: module.id, level: LogLevel.DEBUG } },
    })));
}
```

The node answers each one with that module's `LogModuleInfo` as it stands,
and the DEBUG lines of those modules start arriving as `Log` messages.

## Choices, deliberately simple

- Text formatting is `vsnprintf` alone: no fields, no colors (a sink may
  add its own), no file sink.
- The table is paged rather than bounded to one frame, one page per
  request, and the gateway's copy is capped at 32 modules per node (`gateway.options`): with 33 nodes
  the `NodeTable` has to stay under nanopb's 64 kB struct limit.
- The transport library depends on nothing and therefore does not log: its
  UDP link stopped printing to stderr and exposes `loopbackFallback()`
  instead, which the hub logs once as a WARN; a failed socket call is a
  `false` from `init()` that the application logs.
- Periodic status lines (the board's two per second) are DEBUG: silent by
  default, one `set` away.
