# hub

The gateway between the transport and the browser: one transport node
(kind `gateway`, one `UdpLink`, it beacons like the others and relays
nothing) whose websocket clients see every frame the node hears and can
send frames back. It decodes nothing on their behalf: the pages carry
their own generated codec of `mark4.proto` and read the same `Envelope`
the drones emit. What the hub owns is what a browser cannot: the `.ota`
bundle on disk, the tuning profiles on disk, and the node table with its
frame counters and every node's log module table.

That same TCP port also serves the static pages: the library dispatches on
the `Upgrade` header, so a page loaded from the hub reaches it back with
`new WebSocket("ws://" + location.host)` and never learns a port of its own.

It links the `protocol` (both schemas), `transport`, `log` and `hub_core`
libraries and nothing else: never `flight-core`, never `platform`. Desktop
only. It logs like every node (`gateway/core`, `gateway/ws`, `app/main`
modules): on its stdout, and as `Log` envelopes on the transport from its
own node id, mirrored to its clients as frames from itself.

## Building and running

```sh
cmake --preset desktop && cmake --build --preset desktop
./software/build/desktop/hub/hub
```

The hub takes **no arguments**. It serves with its built-in defaults
(endpoint on 127.0.0.1:47810, transport discovery port 47820, profiles in
`profiles/`, pages in `software/hub/pages/dist` resolved from the binary
location, beacon name `hub-<hostname>`). A flight process reaches it by
beaconing on the discovery port; nothing is wired by hand, nothing is
"connected": every node the transport hears is in the table, and a page
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
location (the repository ignores `logs/`, so a recording is bench output
and never source). The directory need not exist: the first save creates it.

| route | method | what |
|-------|--------|------|
| `/api/telemetry/sessions` | GET | JSON array of `{name, bytes, modified}`, one per stored session, by name. |
| `/api/telemetry/sessions/<name>` | GET | the stored session document. |
| `/api/telemetry/sessions/<name>` | PUT | stores it as `sessions/<name>.telemetry.json`. The body must parse as JSON; it is never interpreted, the shape is the page's business. Answers `{name, bytes}`. |
| `/api/telemetry/sessions/<name>` | DELETE | removes it. |
| `/api/telemetry/exports/<name>.csv` | GET | serves `exports/<name>.csv` as a download (`Content-Disposition`), so a browser saves it under that name. |
| `/api/telemetry/exports/<name>.csv` | PUT | stores the CSV the page built, as it comes: the hub never parses it. |
| `/api/telemetry/configs[/<name>]` | GET, PUT, DELETE | the named view configs, small JSON files under `configs/`. Same rules as the sessions. |

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
page side). Text frames are ignored. There is no JSON anywhere.

Gateway to client:

| body | when | what |
|------|------|------|
| `frame` | every payload the transport delivers | `src` node id, `payload` = one encoded `Envelope` (telemetry, tuning answers, log lines, OTA answers, announces: whatever the node sent). `dst` is left 0: the transport does not report it, and a delivered frame was for the gateway or for everyone anyway. |
| `nodes` | every second, on every table change, on connect | `NodeTable`: the gateway itself first (address empty), then every node the transport hears: id, IPv4 `address`, `port`, `last_seen_ms_ago`, `received` / `lost` / `duplicates` frame counters, its last `Announce` when it has beaconed, and its `log_modules` (the last `LogModules` table it published, whole; the gateway queries a node the moment it appears, so a client connecting late still knows every module and level). |
| `node_telemetry` | on every change of one node's table, on connect | `NodeTelemetry`: one drone node's whole telemetry table, `{id, name, unit}` per measure, as the gateway pulled it page by page (`TelemetryListRequest` / `TelemetryDescriptors`, unicast). The pull starts on the node's first `Announce`, because that is where its kind and its schema are known: only `DRONE_SIM` and `FIRMWARE` are asked, and never a node whose `wire_hash` differs. A page that goes unanswered is asked again every 500 ms, six times, then given up on with one WARN. A node that goes down publishes an empty table: the ids of a table are only stable while the node runs, so a client must drop its curves rather than rebind them to whatever the next boot numbers the same way. Its own message and not a `Node` field: every body of the `GatewayMessage` oneof shares one nanopb struct, and a table per node inside `NodeTable` would cost every message, the per-frame one included, a few hundred kB. |
| `status` | every second, on connect | `GatewayStatus`: the gateway's node id, `wire_hash` (of `mark4.proto` as built), `clients`, `rc_clients` (clients that sent an Rc frame within 2 s), `frames_in`, `frames_out`, `dropped`, `bad_frames`. |
| `ota_state` | on every change of the update client, on connect | phase, verdict and its sentence, `target_node`, `target_slot`, the loaded bundle's identity, what the board last said (slots, running / active slot), transfer progress in bytes. |
| `profiles` | answering `LIST` and `SAVE` | the profile names on disk. |
| `profile` | answering `LOAD` | one profile: name and `TuningSet` pairs. |
| `ack` | answering any client message whose `id` is not 0 | `ok`, `error`; `GatewayMessage.id` echoes the client's. Acks are broadcast to every client: a client correlates on the id it drew and ignores the rest. |

Client to gateway:

| body | what the gateway does |
|------|-----------------------|
| `frame` | `Transport::send(dst, payload)`: a unicast to that node id, or a broadcast when `dst` is 0. The payload is not decoded (its first byte is compared with the `Rc` tag to count pilots). Refused when the node is unknown. This is how RC, tuning, scenarios, reboots and `LogControl` travel; a `LogControl` addressed to the gateway's own node id is handled by the gateway (it does not forward to itself). |
| `ota_command` | `START` (bundle path, empty = the build output), `ABORT`, `REVERT`, `STATUS_REQUEST`, each naming `target_node`. The target is fixed for the whole session: while a session runs, a command naming another node is refused; `ABORT` always works. |
| `profile_command` | `LIST`, `SAVE` (name + values), `LOAD` (name), `PUSH` (name + `target_node`: one `TuningSet` frame per value). Names are letters, digits, `_` and `-`. |

A client that connects gets `nodes`, `status` and `ota_state` immediately.
The wire mismatch of a node is not a field: a page compares
`Node.announce.wire_hash` with `GatewayStatus.wire_hash`; the hub also
logs the mismatch once (a `gateway/core` WARN) when the announce arrives.

Simplifications, deliberate: the gateway's own beacon is not replayed as a
`frame` (the gateway is the first entry of `nodes`, with its Announce);
`Frame.dst` is 0 on delivered frames; the Ack carries its id on the
enclosing message only; there is no gateway-level log message (field 31 of
`GatewayMessage` was one and stays reserved): the gateway's lines are
`Log` envelopes in frames from its own node id, like everyone else's.
`gateway.proto` is not part of `WIRE_HASH`: the pages are built from the
same tree as the hub that serves them.

## Firmware update

An `OtaCommand.START` sends one `.ota` bundle to the target node over the
transport, as unicasts: the updater messages are one more body of the same
envelope on the same link, so telemetry keeps flowing between them and the
ESP32 relay forwards them like any other unicast for the board. A
`drone_sim` is a valid target too, with its emulated flash.

`bundle_path` is optional and defaults to
`software/build/stm32/drone_firmware/drone_firmware.ota`, resolved from the
hub binary: the common case is one click after a build. Loading validates the
bundle against itself (magic, wire hash, announced sizes and CRC-32,
and each image header against the manifest entry describing it) and then
against the board (right chip, an image for the inactive slot, an image that
fits a slot). Only then does a byte go out.

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
  ignores the rest. This keeps the endpoint free of any per-client state
  shared between the library threads and the poll loop.
- The endpoint has no authentication. It binds the loopback interface, and
  it is a bench tool on a trusted network.
- The hub relays nothing: with one link there is nothing to relay between.
  Two hubs on one LAN both hear every broadcast and both beacon; the board
  learns both through its relay.
- Tuned values do not survive a simulator reset: `drone_sim` rebuilds its
  flight core on the reset (there is no state a teleport could keep) and
  does not re-announce, so the hub has no event to push a profile on. Push
  it again explicitly with `ProfileCommand.PUSH` after resetting the world.
- POSIX only (`/proc/self/exe`, `poll`).
