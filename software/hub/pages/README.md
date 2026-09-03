# Hub pages

The web front end the hub serves: two windows meant for two screens,
`index.html` (control: one widget per drone node and the 3D attitude, the
default view) and `plots.html` (telemetry: the lanes, and the sessions
recorded into them). The entry keeps the `plots` name because the editor
extension embeds it as a webview; the page itself is the telemetry page. They show the drones, not the
system: the inventory of transport nodes and the log stream belong to the
editor extension (`tools/vscode-mark4`), so the pages never list a node
that is not a drone. Plain TypeScript bundled by
esbuild into `dist/`, one ESM bundle per page plus a single `style.css`: no
framework, no runtime template engine, nothing for the hub to do beyond
handing out static files.

```sh
pnpm install
pnpm build       # gen + dist/, minified, what the hub serves
pnpm watch       # gen + rebuild on change, with sourcemaps
pnpm typecheck
pnpm test
pnpm smoke       # against a running hub, see scripts/smoke.ts
```

## Generated codecs

`pnpm gen` (run by every script above) generates `src/gen/mark4_pb.ts` and
`src/gen/gateway_pb.ts` from the two schemas in
`software/components/protocol/` with `protoc-gen-es` (`@bufbuild/protoc-gen-es`,
runtime `@bufbuild/protobuf`), driven by the `protoc` bundled in the python
package `grpcio-tools` that the C++ build already needs. `src/gen/` is
gitignored: nothing generated is committed, the pages are always built from
the same schema as the hub that serves them. `uint64` fields come out as
`bigint` (`Number(telemetry.timestampUs)` where a plain number is wanted).

## Layout

- `src/shared/` - modules every page uses: the binary websocket link
  (`gateway_socket.ts`: `GatewayMessage` both ways, per-case handlers,
  `onEnvelope` for decoded frames, ack correlation), the node model
  (`nodes.ts`), the page shell (`shell.ts`, a thin top bar with the page's
  own controls, the connection state and the toasts), the quaternion helpers
  (`quat.ts`) and the flight-core state words (`phases.ts`).
- `src/lanes/` - the lane viewer: time-axis math, sample buffers, the uPlot
  charts and the ruler. It knows what a plotted series looks like
  (`SeriesDef`) and nothing about where the values come from.
- `src/telemetry/` - the telemetry page's own model: the selection and its
  buffers (`model.ts`), the stored session, view config and CSV shapes
  (`session.ts`), the hub's HTTP store (`store.ts`) and the configuring
  panel (`config_panel.ts`). Everything but the panel is DOM-free and
  unit tested.
- `src/console/` - the control window: one `DroneWidget` per live drone
  node (`drone_widget.ts`), created when the node table lists a
  `firmware` or `drone_sim` node and destroyed when it leaves, transmitter
  parked safe on the way out. Observation is the same for every nature
  (phase, throw detector, altitude, motors); the controls are the
  kill/arm/mode switches and a throttle slider, the folded scenario block
  (drone_sim only) and the tuning table with the gateway's profiles.
  Its header is the node name as title, the kind as a badge, the node id in
  8 hex digits, muted, and a WIRE MISMATCH badge when the node was built on
  another schema than the gateway; a node the gateway has not heard from for
  1.5 s dims until it comes back or leaves. No age is ever written out.
  `rc.ts` is the piloting state, pure and unit tested. `ota_panel.ts` is
  the firmware update panel, over the pure `ota.ts`; its target is a node
  picked in the panel.
- `src/<page>/main.ts` - one page. Every directory holding a `main.ts`
  becomes its own bundle, so adding a page is adding a directory and an
  `.html` file next to `esbuild.js`.
- `test/` - node:test suites run through tsx.
- `scripts/smoke.ts` - the bench smoke: a `ws` client using the same
  generated codec against a live hub, plant and two flight processes.

## Talking to the gateway

The pages are served by the hub, so the websocket URL is always
`ws://<the host that served this page>`. There is nothing to configure.

Every message is one binary `GatewayMessage` (`gateway.proto`, see
`software/hub/README.md` for the contract). Handlers are registered per body
case and an unknown case is ignored, so a gateway that learns a new message
never breaks an older page. A transport frame carries one `Envelope` of
`mark4.proto`: `socket.onEnvelope((src, envelope) => ...)` hands it decoded
with the node id it came from, and `socket.sendEnvelope(dst, envelope)` /
`socket.requestEnvelope(dst, envelope)` send one to a node.

Acks are broadcast to every connected client, so a correlation id has to say
which tab asked. Each tab draws a random nonce at load and numbers its
requests inside that block (`id = nonce * 65536 + counter`, never 0); an ack
whose id is not in the map belongs to another tab and is dropped.

## The node model

`shared/nodes.ts` is the world of a page: the gateway's `NodeTable` (every
node id the transport hears, its last Announce, its age and counters),
refreshed by the frames that arrive between two tables, and the gateway's
own wire hash from `GatewayStatus`. Everything on screen is keyed by node
id: a widget for every drone, a source entry for every drone in the plots
and update selectors (`nodeLabel`, the name then the 8 hex digits of the
id). Colors are a stable hash of the node id, so a drone wears the
same color in every tab. `onChange` reports the diff (nodes added, ids
removed, a kind change counting as both), which is exactly the widget
lifecycle, and carries the fresh views the widgets repaint from;
`test/nodes.test.ts` covers it, two `drone_sim` staying two entries, the
header fields and the drones-only selector list included.

The top bar shows nothing of that table. The one thing it keeps besides the
connection state is the multi-pilot warning: it is a safety matter, not
inventory.

## Exact state

The estimated attitude is the `Status` envelope; the exact plant state, when
the sender has a plant, is its `truth` field, sampled at the same instant.
The error angle a page shows is computed page-side from the two quaternions
of that one message: a readout, not a score. The 3D ghost is the truth of
whichever live drone streams one. The same comparison exists as a telemetry
measure (`sim/attitude_error`), computed drone-side, which is the one to
plot: it is sampled at the loop rate rather than at the 50 Hz of Status.

## Piloting

The transmitter of a drone widget is its switches (kill, arm, mode) and its
throttle slider: no engage ritual, no keyboard layer. The widget streams an
`Rc` envelope to its node at 10 Hz from the first interaction on and never
stops while the page is visible.

**The silence is the fail-safe**: the drone cuts on its own RC timeout, so a
closed tab, a frozen browser or a dead link all end the same way. Hiding the
page flips the kill switch on - a pilot who cannot see the drone is not
piloting it - and a widget leaving (its node disappeared) sends the safe
state twice before its stream stops. The gateway counts the clients that
streamed RC recently and the top bar warns when there is more than one.

## Firmware update

The update panel sits under the drone list on the control page: reflashing
is an operation on a node of the table, and the panel has to stay readable
while the board reboots and its widget momentarily disappears. Its target
selector lists the drones (a `firmware` node first, else the first
`drone_sim`, whose emulated flash takes an update too); every `OtaCommand`
names that node.

It shows the slot rows (state, image identity, which slot runs and which
boots next, the revert on the other valid slot), the bundle path prefilled
with the build output the gateway defaults to, the phase in plain words, a
progress bar following the bytes the board acknowledged writing, and the
verdict sentence the gateway wrote. The gateway owns the whole state
machine and publishes it as one `OtaState` on every change, so the panel
derives nothing: it paints the last message. While nothing is running it
asks for a fresh board status every three seconds.

## The telemetry page

There is no catalog of series in the pages. A drone publishes its own table
of measures (`software/components/telemetry/README.md`), the gateway pulls
it and republishes it as `NodeTelemetry`, and the page offers exactly that:
adding a measure to the firmware is one line there and nothing at all here.

**Source node**: the existing drones-only selector, exactly one selected.
While its table is empty the panel says the gateway is still asking.

**Three phases** drive what the page does:

- `configuring`: a filter box over the measure names, one row per measure
  with a checkbox, its colour dot and its unit, a period in ms clamped to
  [1, 5000] (default 50) that shows what the node acknowledged once the ack
  arrives, and a layout section listing the lanes in order with one
  draggable chip per series. Drop a chip on another lane to group them -
  only among measures of the same unit, and the lanes that cannot take it
  grey out while the chip is in the air, because a shared y axis is only
  honest between measures that read alike. Drop it on the "new lane" target
  to split it out again, and drag a lane by its grip to reorder. A new
  selection opens in a lane of its own, in the first unused hue.
- `recording`: the config is frozen. The page sends `TelemetryEnable` to
  the node at the start and again every second, because the enable IS the
  keepalive: the drone stops three seconds after the last one, so a tab that
  crashes never leaves a board streaming to nobody. It sends `period_ms = 0`
  once on stop, on a node switch and on `pagehide` / `beforeunload`, so the
  usual case costs nothing at all. Follow and pause behave as they always
  did.
- `viewing`: no traffic. Browse, save, export.

**Identity is the name.** A descriptor id is an index into the node's frozen
table and a reboot hands the same number to another measure, so a series is
bound by name and only ever routed by id. When the node's table changes the
routes are rebuilt from it: a measure that is gone is flagged absent and fed
nothing, and one whose name is back is rebound to whatever id it now has -
with the enable re-sent when a recording is running. A node that leaves the
table, or goes silent for three seconds while recording, pushes an explicit
`null` gap marker so the lanes break instead of drawing a chord across the
hole.

**Sessions** are files on the hub, not in the browser: `PUT`, `GET` and
`DELETE` under `/api/telemetry/sessions` (see `software/hub/README.md`), so
a recording outlives its tab and opens from another machine. The document
is `version: 1` and carries the node it came from, `t0Us`, the duration, the
period and every series with its samples and its gap markers. **CSV export**
is long format (`series,unit,t_s,value`, gap markers skipped), built here
and stored under `/api/telemetry/exports`; the page then offers the link the
hub serves it from.

**View configs** are what is ticked and how it is laid out, with no data in
them. Named ones live on the hub (`/api/telemetry/configs`: save, load,
delete). The working config - what is ticked right now - is auto-saved to
`localStorage` under `mark4.pages.telemetry.v1`, debounced, and restored on
load. This replaces the old per-browser `mark4.pages.viewConfigs` key, which
is gone: a layout naming a hardcoded series catalog has nothing left to
name.
