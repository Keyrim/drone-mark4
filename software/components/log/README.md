# log

The one logging library of every node: the board, `drone_sim`, the hub, the
ESP32 relay.
A source file declares its module once, logs printf-style through it, and
every line carries the module, a level and a timestamp to up to two sinks:
a console (stdout on a desktop, RTT on the board) and the transport, where
it travels as a `mark4.Log` envelope so the pages and any node script read
it. Levels are per module and set at runtime, from the process or from the
wire.

```cpp
#include "log/module.hpp"
#include "log/module_ids.hpp"

namespace
{
    mark4::LogModule MODULE{mark4::LOG_MODULE_PLATFORM_IMU, "platform/imu"};
}

MODULE.info("found at 0x%02X", address);   // also trace / debug / warn / error
```

Two CMake targets. `log` is a leaf (it links `drone_warnings` alone, no
heap, no iostream, no `std::function`, plain C++17) and builds on every
preset; on desktop it also holds `ConsoleSinkPosix`. `log_wire` adds what
the wire needs (`log/wire.hpp`: `TransportSink`, `logPublishModules()`,
`logHandleControl()`) and links `protocol/`. The RTT sink lives next to the
RTT driver, `platform_stm32/rtt_sink.hpp`.

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
- `TransportSink` (any node, `log_wire`): encodes a `Log` envelope and hands
  the bytes to a `LogSendFn` function pointer the application registers
  (a transport broadcast for the flight processes; the hub also mirrors
  them to its websocket clients as frames from itself). Rate limited to
  `MAX_LINES_PER_SECOND` = 50; the lines refused are counted and the count
  goes out once per second as a WARN of `log/core`, written straight to the
  wire so it cannot be rate limited itself.

The timestamp comes from a clock function pointer registered once
(`logSetClock(fn, context)`): the library never reads a clock, like
flight-core and transport. Until it is registered, records carry 0 (the
board's first lines, before its timer runs).

The library is not thread-safe. Every node of the project logs from its
one loop thread; the hub's websocket threads log nothing.

## Wire

Three messages of `mark4.proto`, one `Envelope` body each:

- `Log { timestamp_us, level, module_id, text (96) }`, broadcast by the
  node, one per line let through.
- `LogModules { start_index, total, modules (8 at most) }` with
  `LogModuleInfo { id, name (32), level }`: the node's table, in pages of
  at most 8 modules because 48 names of 32 characters do not fit one
  512-byte frame. A node publishes it once after its first beacon, again on
  every level change, and on request. A page opening at `start_index` 0
  restarts the table on the receiving side.
- `LogControl { oneof { bool query; LogModuleLevel set { module_id, level } } }`,
  ground to node: `query` asks for the table, `set` moves one module. Every
  node handles it (drone_sim, the firmware, the hub for its own modules)
  through `logHandleControl()`, which returns true when the table must be
  published again.

Prefix semantics live in the client: it knows the names from the table and
sends one `set` per matching id. The node stays dumb: no string matching
on the wire, no dynamic registration.

The gateway remembers the last table of every node it hears (dropped with
the node) and exposes it as `Node.log_modules` in the `NodeTable`, so a
client connecting late knows every module and level without asking. A node
that booted before the gateway published its table into the void, so the
gateway sends one `query` to every node the moment it appears. The pages
toast WARN and ERROR lines only, prefixed with the module name resolved
from that table (`#id` when the table has not arrived).

## Setting a level from a client (TypeScript, the generated codec)

```ts
// modules = node.logModules from the NodeTable; prefix "platform/"
for (const module of modules.filter((m) => m.name.startsWith("platform/"))) {
    socket.send(frameMessage(nodeId, create(EnvelopeSchema, {
        body: { case: "logControl", value: { request: { case: "set",
                value: { moduleId: module.id, level: LogLevel.DEBUG } } } },
    })));
}
```

The node answers each `set` that changed something with its whole table
(the pages), and the DEBUG lines of those modules start arriving as `Log`
frames.

## Choices, deliberately simple

- Text formatting is `vsnprintf` alone: no fields, no colors (a sink may
  add its own), no file sink.
- The table is paged rather than bounded to one frame, and the gateway's
  copy is capped at 32 modules per node (`gateway.options`): with 33 nodes
  the `NodeTable` has to stay under nanopb's 64 kB struct limit.
- The transport library depends on nothing and therefore does not log: its
  UDP link stopped printing to stderr and exposes `loopbackFallback()`
  instead, which the hub logs once as a WARN; a failed socket call is a
  `false` from `init()` that the application logs.
- Periodic status lines (the board's two per second) are DEBUG: silent by
  default, one `set` away.
