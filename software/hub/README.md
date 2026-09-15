# hub

The gateway between the transport and the browser: one transport node
(kind `gateway`, one `UdpLink`, it keeps alive like the others and relays
nothing) that decodes everything it hears and publishes it to its websocket
clients as typed messages. Nothing raw crosses: a client never sees an
`Envelope` and never sends one, it sends typed commands the gateway carries
out through one consumer. What the hub owns besides that is what a browser
cannot: the `.ota` bundle on disk, the tuning profiles on disk, and the node
table with its counters and every node's identity (asked for, kept in a
`DiscoveryDirectory`).

That same TCP port also serves the static pages: the library dispatches on
the `Upgrade` header, so a page loaded from the hub reaches it back with
`new WebSocket("ws://" + location.host)` and never learns a port of its own.

It links the `protocol` (both schemas), `transport`, `messaging`,
`discovery`, `log`, the four consumers, `ota_consumer` and `hub_core` and
nothing else: never `flight-core`, never `platform`. Desktop only. The
transport is polled through a `Messenger`, and what the gateway knows it
knows through one consumer per concept (`StatusConsumer`, `LogConsumer`,
`TelemetryConsumer`, `TuningConsumer`, each sized at
`Transport::MAX_NODES`). A consumer names the kinds that carry its
provider, so the gateway names none: it opens a node when the directory
learns its identity, subscribes, pulls the tables one page at a time and
drops everything of a node that goes down.

One gateway per concept sits on each consumer, one file per concept
(`gateway_status`, `gateway_log`, `gateway_telemetry`, `gateway_tuning`,
plus `gateway_pilot`, which has no consumer behind it because `Rc` is a
stream and not a state). Each listens to its consumer, publishes what it
holds through the composition (`AbsGatewayPublisher`: `broadcast()` and
`sendTo()`), and carries out the commands of its own concept. Two handlers
of the composition complete them: `OtaReader`, which feeds the update
consumer the `OtaStatus` / `OtaAck` / `OtaChunkAck` it waits for (that
consumer was moved as it was and is not a handler itself), and
`HubRequester`, which claims no tag and exists to own the requests a node
command sends (a reboot, a scenario). The directory is the handler of
`IdentityRequest` and `Announce`.

It logs like every node (`gateway/core`, `gateway/ws`, `app/main`
modules): on its stdout, and as `Log` lines to whoever subscribed to it. A
messenger refuses this node as a destination, so its own lines reach its
clients through the local sink of its `LogProvider`, which is the log
gateway: they appear as `NodeLogLines` from its own node id, exactly like
every other node's.

## Building and running

```sh
cmake --preset desktop && cmake --build --preset desktop
./software/build/desktop/hub/hub
```

The hub takes **no arguments**. It serves with its built-in defaults
(endpoint on 127.0.0.1:47810, transport discovery port 47820, profiles in
`profiles/`, pages in `software/hub/pages/dist` resolved from the binary
location, own name `hub-<hostname>` in the node table). A flight process
reaches it by its keepalives on the discovery port; nothing is wired by
hand, nothing is "connected": every node the transport hears is in the
table, and a page
commands whichever node id it wants. A default worth changing is a
compile-time change in `transport/udp_link.hpp` or `HubApp::Config`, not a
flag.

The hub never starts Godot or a flight process: both are yours to run and
restart at will (Godot from its own terminal or the "godot sim" VS Code
task, `drone_sim` from anywhere). Godot is one more node of the LAN, kind
`plant`, and spawns one virtual drone per `drone_sim` it hears; discovery
picks each incarnation up within a second, so the hub is the process that
stays up for the whole bench session. Two `drone_sim` are two nodes with
two ids and two widgets.

## Pages

`GET /` serves `index.html` from the pages directory, `GET /<path>` the file
at that path below it. The hub resolves `software/hub/pages/dist` from its own
location, falling back to that relative path. A missing directory is one log
line at startup and a 404 per request, never a startup failure.

The Content-Type comes from the extension (`.html`, `.js`, `.mjs`, `.css`,
`.svg`, `.json`, `.csv`, `.ico`, `.png`; anything else is an opaque byte
stream), and every response carries `Cache-Control: no-store`. A URI holding
a `..` component is refused: nothing outside the pages directory is
reachable.

## HTTP API: the telemetry store

The HTTP side stays filesystem-only, which is the invariant that keeps the
hub lock-free: these handlers run on the websocket library's connection
threads and touch no registry and no counter. They read and write files
under the telemetry directory, `logs/telemetry` resolved from the hub's own
location (the repository ignores `logs/`, so an export is bench output and
never source). The directory need not exist: the first save creates it.

| route | method | what |
|-------|--------|------|
| `/api/telemetry/configs` | GET | JSON array of `{name, bytes, modified}`, one per stored view config, by name. |
| `/api/telemetry/configs/<name>` | GET | the stored config document. |
| `/api/telemetry/configs/<name>` | PUT | stores it as `configs/<name>.json`. The body must parse as JSON; it is never interpreted, the shape is the page's business. Answers `{name, bytes}`. |
| `/api/telemetry/configs/<name>` | DELETE | removes it. |
| `/api/telemetry/exports/<name>.csv` | GET | serves `exports/<name>.csv` as a download (`Content-Disposition`), so a browser saves it under that name. |
| `/api/telemetry/exports/<name>.csv` | PUT | stores the CSV the page built, as it comes: the hub never parses it. |

A name becomes a file name, so it obeys one rule: 1 to 64 characters of
letters, digits, `_` and `-`. No separator, no dot, no leading dot, nothing
that could name a file outside its own directory; anything else is a 400.
Every body is capped at 64 MiB. A wrong method on an existing route is a
405, an unknown route a 404, and so is every telemetry route when the hub
was given no telemetry directory.

## Websocket contract: gateway.proto

Every websocket message, both directions, is one binary
`mark4.GatewayMessage` of `software/components/protocol/gateway.proto`
(nanopb on the hub side, `hub/gateway_codec.hpp`; protoc-gen-es on the
page side). Text frames are ignored. There is no JSON anywhere, and no
encoded `Envelope`: the types a client needs of `mark4.proto` are the ones
`gateway.proto` imports (`Status`, `TelemetryDescriptor`, `TelemetryData`,
`LogModuleInfo`, `Log`, `Announce`, `Rc`, `TuningInfo`, `TuningAck`,
`SimScenario`).

Gateway to client, the state the gateway holds: on every change, and whole
to a client that connects.

| body | what |
|------|------|
| `nodes` | `NodeTable`: the gateway itself first (address empty), then every node the transport hears: id, IPv4 `address`, `port`, `last_seen_ms_ago`, `received` / `lost` / `duplicates` counters, and its `Announce` once the directory has it (every node that appears is sent an `IdentityRequest`, again every 500 ms up to five times; a node that never answers shows without identity). Every second and on every change. |
| `node_log_modules` | `NodeLogModules`: one node's whole module table with the level of each, as its `LogConsumer` pulled it; empty for a node that went down. The gateway's own table is published the same way, from its own registry. |
| `node_log_lines` | `NodeLogLines`: the lines of one node as they arrive, one per message, and on connect the last 256 lines the gateway kept per node, oldest first. The gateway's own lines come from its own node id. |
| `node_status` | `NodeStatus`: one node's last report, at that node's own cadence. |
| `node_telemetry` | `NodeTelemetry`: one node's whole measure table, `{id, name, unit}` per measure, as its `TelemetryConsumer` pulled it page by page. A node that goes down publishes an empty table: the ids of a table are only stable while the node runs, so a client must drop its curves rather than rebind them to whatever the next boot numbers the same way. |
| `node_telemetry_config` | `NodeTelemetryConfig`: the configuration as the node applied it (the ids it kept, the period it clamped) and whether the gateway holds its sample stream. |
| `telemetry_samples` | `TelemetrySamples`: one sampling instant, to the clients marked on that node and to them alone. |
| `node_tuning` | `NodeTuning`: one node's whole parameter table. |
| `tuning_result` | `TuningResult`: the answer to a write or a read, to every client; a client correlates on the id of its own command and on the parameter id. |
| `status` | `GatewayStatus`: node id, `wire_hash` (of `mark4.proto` as built), `clients`, `rc_clients` (pilot seats held), `messages_in` (payloads the transport delivered), `commands` and `refused` (client commands carried out and refused), `dropped`. Every second. |
| `ota_state` | phase, verdict and its sentence, `target_node`, `target_slot`, the loaded bundle's identity, what the board last said (slots, running / active slot), transfer progress in bytes. |
| `profiles` | answering `LIST` and `SAVE`: the profile names on disk. |
| `profile` | answering `LOAD`: one profile, name and `TuningSet` pairs. |
| `ack` | answering any client message whose `id` is not 0: `ok`, `error`; `GatewayMessage.id` echoes the client's. Acks are broadcast to every client: a client correlates on the id it drew and ignores the rest. |

Client to gateway, every one a command the gateway carries out through one
consumer and answers with an `Ack` when `id` is set.

| body | what the gateway does |
|------|-----------------------|
| `telemetry_command` | `config { ids, period_ms }`: the configuration to the node, last writer wins between clients too (there is one configuration per node). `subscribe { bool }`: marks this client; the gateway holds the node's stream while at least one client is marked and gives it back when the last one clears or disconnects. |
| `log_command` | `refresh {}`: the module table pulled again. `set_level { module_id, level }`: one level moved, on the node named or, when that is the gateway's own id, in its own registry. |
| `tuning_command` | `refresh {}`: the parameter table pulled again. `set { id, value }` / `get { id }`: one request, answered by a `tuning_result`. |
| `pilot_input` | one `Rc` forwarded to the node named, from the gateway's own id (see below). |
| `node_command` | `reboot {}` or `scenario { SimScenario }`: one request to the node named. |
| `ota_command` | `START` (bundle path, empty = the build output), `ABORT`, `REVERT`, `STATUS_REQUEST`, each naming `target_node`. The target is fixed for the whole session: while a session runs, a command naming another node is refused; `ABORT` always works. |
| `profile_command` | `LIST`, `SAVE` (name + values), `LOAD` (name), `PUSH` (name + `target_node`: one parameter write per value). Names are letters, digits, `_` and `-`. |

A client that connects gets everything: the counters, the update state,
every node's status, module table and log ring, every measure and
parameter table, and the node table. The wire mismatch of a node is not a
field: a page compares `Node.announce.wire_hash` with
`GatewayStatus.wire_hash`; the hub also logs the mismatch once (a
`gateway/core` WARN) when the directory learns the identity.

### Piloting

The gateway is the pilot node: it forwards each `pilot_input` as one `Rc`
to the node named, from its own id, at the cadence the client sends. Two
rules the fail-safe depends on:

- **It never repeats.** A client that stops sending stops the stream, and
  the node's own RC timeout does the rest. The gateway holds no last state
  to resend.
- **One pilot per node at a time.** The first client to send takes the
  seat; another client's input is refused (`Ack`) while the seat is held;
  the seat is released when the holder disconnects or has sent nothing for
  `RC_PILOT_WINDOW_US` (2 s). `GatewayStatus.rc_clients` counts the seats
  held.

Simplifications, deliberate: the gateway's own Announce is sent only to a
node that asks for it (the gateway is the first entry of `nodes`, with
it); the Ack carries its id on the enclosing message only; field 1 of
`GatewayMessage` was the raw `Frame` and field 31 a gateway-level `Log`,
both reserved for good. `gateway.proto` is not part of `WIRE_HASH`: the
pages are built from the same tree as the hub that serves them.

## Firmware update

An `OtaCommand.START` sends one `.ota` bundle to the target node over the
transport, as unicasts: the updater messages are one more body of the same
envelope on the same link, so telemetry keeps flowing between them and the
ESP32 relay forwards them like any other unicast for the board. A
`drone_sim` is a valid target too, with its emulated flash.

`bundle_path` is optional and defaults to
`software/build/stm32/drone_firmware/drone_firmware.ota`, resolved from the
hub binary: the common case is one click after a build. The ESP32 relay is
a valid target too, with `esp32-bridge/build/esp32_bridge.ota` typed as the
path: its bundle holds one image for both slots (an ESP-IDF image is
position-independent across the OTA partitions) and the relay runs the same
updater over its partitions. Loading validates the bundle against itself
(magic, wire hash, announced sizes and CRC-32, and each image against the
manifest entry describing it: its `OtaImageHeader` for the STM32s and the
sim, the ESP-IDF magic for the relay) and then against the board (right
chip, an image for the inactive slot, an image that fits a slot). Only then
does a byte go out.

The session then walks one phase at a time, and every change is published as
one `ota_state` message, which is what makes a progress bar move without
anybody polling:

```
IDLE -> QUERY -> ERASING -> TRANSFER -> VERIFYING -> REBOOTING
     -> WAITING_BOARD -> TESTING -> CONFIRMED
```

The transfer is go-back-N: chunks of at most 240 bytes at strictly increasing
offsets, at most 16 in flight, one cumulative `nextOffset` acknowledgement per
window. A 500 ms acknowledgement silence resends from the last acknowledged
offset; a bounded number of those and the session fails with the offset it
died at. Chunks are paced 3 ms apart, because the hub reaches the board over
WiFi and the board over a 921600 baud UART, and the poll loop tightens to 1 ms
for the duration so that pacing is the throttle rather than the sleep.

`progress.acked_bytes` is what the board has written and is what a bar must
show; `sent_bytes` runs up to one window ahead of it and goes backwards on a
resend.

After the reboot the hub polls `OtaStatusRequest` once a second until the
board answers again, ignoring answers for the first 1.5 s (the old image can
still answer one request between the command and the reset). What comes back
decides the verdict, and the verdict is a sentence in `verdict_text`:

- the bundle's build epoch, on a slot reported `testing`: the trial boot
  worked. The image confirms itself on the first request it serves, so the
  hub keeps polling until the slot reads `valid`, which moves the phase to
  `CONFIRMED`; nothing is sent to confirm.
- the git hash it ran before: the bootloader rolled back. Phase
  `ROLLED_BACK`, and nothing is confirmed.
- neither: phase `FAILED`, saying what it found.

`REVERT` asks the board to activate its other slot and reboots it; it is
legal while a trial image runs, which is exactly when it is wanted.
`ABORT` drops the session and tells the board so its half-written slot is
released now rather than at its own timeout. Every refusal the board sends
(`DENIED_ARMED`, `CRC_MISMATCH`, ...) comes back as the sentence behind the
code, never as the code.

## Known limitations

- Acks are broadcast to every connected client rather than sent back to the
  one that asked: a client correlates the answer with the `id` it sent and
  ignores the rest. The snapshots and the sample streams do go to one
  client, so the endpoint knows its connections; the ack simply never
  needed it.
- The endpoint has no authentication. It binds the loopback interface, and
  it is a bench tool on a trusted network.
- The hub relays nothing: with one link there is nothing to relay between.
  Two hubs on one LAN both hear every broadcast and both keep alive; the
  board learns both through its relay.
- Tuned values do not survive a simulator reset: `drone_sim` rebuilds its
  flight core on the reset (there is no state a teleport could keep) and
  its identity does not change, so the hub has no event to push a profile
  on. Push
  it again explicitly with `ProfileCommand.PUSH` after resetting the world.
- POSIX only (`/proc/self/exe`, `poll`).
