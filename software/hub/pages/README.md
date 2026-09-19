# Hub pages

The web front end the hub serves: `index.html` (control: one widget per
drone node and the 3D attitude, the default view), `plots.html` (telemetry:
a config of measures, and the lanes it is recorded into) and
`transport.html` (the state of the wire itself). The entry keeps the
`plots` name because the editor
extension embeds it as a webview; the page itself is the telemetry page.
The first two show the drones, not the
system: the inventory of transport nodes and the log stream belong to the
editor extension (`tools/vscode-mark4`), so they never list a node
that is not a drone. The transport page is the exception by subject: what
it draws is every node and the links between them. Plain TypeScript bundled by
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

## Components and icons

The transport page is built from the editor's own web components
(`@vscode-elements/elements`) over the codicon font (`@vscode/codicons`,
pinned at `0.0.46-24`: upstream publishes its releases under that
prerelease shape and no plain `0.0.46` exists). The font stylesheet is a
bundle entry point of its own, `dist/codicon.css`, and the page links it
as `<link id="vscode-codicon-stylesheet" ...>`: the icon component looks
that link up by its id and loads the stylesheet it names into its own
shadow root, where a stylesheet the document imports reaches nothing. Only
the page that draws icons links it.

## Layout

- `src/shared/` - modules every page uses: the binary websocket link
  (`gateway_socket.ts`: `GatewayMessage` both ways, per-case handlers, ack
  correlation), the node model
  (`nodes.ts`), the page shell (`shell.ts`, a thin top bar with the page's
  own controls, the connection state and the toasts), the theme relay
  (`theme.ts`), the quaternion helpers
  (`quat.ts`) and the flight-core state words (`phases.ts`).
- `src/lanes/` - the lane viewer: time-axis math, sample buffers, the uPlot
  charts and the ruler. It knows what a plotted series looks like
  (`SeriesDef`) and nothing about where the values come from.
- `src/telemetry/` - the telemetry page's own model: the selection and its
  buffers (`model.ts`), the catalog folded into a tree (`tree.ts`), the
  view config and CSV shapes (`config.ts`), the hub's HTTP store
  (`store.ts`) and the setup view (`config_panel.ts`: the tree of measures
  in `catalog.ts`, the lanes in `lanes_panel.ts`). Everything but the
  views is DOM-free and unit tested.
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
- `src/transport/` - the transport page: where the cards go (`layout.ts`),
  what the verdict says in words (`health.ts`), the graph (`graph.ts`), the
  panel (`panel.ts`) and the page's own stylesheet (`transport.css`,
  emitted as `dist/transport.css` from the `main.ts` that imports it).
  `layout.ts` and `health.ts` are DOM-free.
- `src/<page>/main.ts` - one page. Every directory holding a `main.ts`
  becomes its own bundle, so adding a page is adding a directory and an
  `.html` file next to `esbuild.js`.
- `test/` - node:test suites run through tsx.
- `scripts/smoke.ts` - the bench smoke: a `ws` client using the same
  generated codec against a live hub, plant and two flight processes.
  `scripts/log_smoke.ts` is the same for the log path.

## Talking to the gateway

The pages are served by the hub, so the websocket URL is always
`ws://<the host that served this page>`. There is nothing to configure.

Every message is one binary `GatewayMessage` (`gateway.proto`, see
`software/hub/README.md` for the contract). Handlers are registered per body
case (`socket.on("nodeStatus", ...)`, `socket.off(...)` when a widget leaves
with its node) and an unknown case is ignored, so a gateway that learns a
new message never breaks an older page.

**Nothing raw crosses.** A page never encodes or decodes an `Envelope`: the
gateway decodes everything it hears through its consumers and publishes
typed messages (`NodeStatus`, `NodeLogLines`, `NodeLogModules`,
`NodeTelemetry`, `NodeTelemetryConfig`, `TelemetrySamples`, `NodeTuning`,
`TuningResult`), and a page sends typed commands naming the node they act on
(`TelemetryCommand`, `LogCommand`, `TuningCommand`, `PilotInput`,
`NodeCommand`, plus the gateway-local `OtaCommand` and `ProfileCommand`).
The generated `mark4_pb.ts` is kept for the types `gateway.proto` imports
and for nothing else.

Acks are broadcast to every connected client, so a correlation id has to say
which tab asked. Each tab draws a random nonce at load and numbers its
requests inside that block (`id = nonce * 65536 + counter`, never 0); an ack
whose id is not in the map belongs to another tab and is dropped.

## The node model

`shared/nodes.ts` is the world of a page: the gateway's `NodeTable` (every
node id the transport hears, its last Announce, its age and counters),
refreshed by what arrives from a node between two tables (its `NodeStatus`,
its `NodeLogLines`), and the gateway's own wire hash from `GatewayStatus`.
A node's log modules are their own message, so the model keeps them beside
the table: a name to resolve a log line with, not a field of a node. Everything on screen is keyed by node
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

The estimated attitude is the `Status` the gateway publishes as
`NodeStatus`; the exact plant state, when the sender has a plant, is its
`truth` field, sampled at the same instant.
The error angle a page shows is computed page-side from the two quaternions
of that one message: a readout, not a score. The 3D ghost is the truth of
whichever live drone streams one. The same comparison exists as a telemetry
measure (`sim/attitude_error`), computed drone-side, which is the one to
plot: it is sampled at the loop rate rather than at the 50 Hz of Status.

## Piloting

The transmitter of a drone widget is its switches (kill, arm), its mode
selector and its throttle slider: no engage ritual, no keyboard layer. The
widget streams a `PilotInput` naming its node at 20 Hz from the first
interaction on and never stops while the page is visible; the gateway
forwards each one as one `Rc` to that node, from its own id, and never
repeats it. The first client to pilot a node holds it: another tab's input
is refused until the seat is released. It has no sticks
and streams them released, which is why it starts in the `level` mode
(released sticks mean level) rather than `manual` (the sticks are body
rates and nothing levels the drone: that mode is flown from a gamepad,
through the phone).

**The silence is the fail-safe**: the drone cuts on its own RC timeout, so a
closed tab, a frozen browser or a dead link all end the same way. Hiding the
page flips the kill switch on - a pilot who cannot see the drone is not
piloting it - and a widget leaving (its node disappeared) sends the safe
state twice before its stream stops. The gateway counts the pilot seats held and the top bar warns when there is
more than one.

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
While its table is empty the catalog says the gateway is still asking.

**Two modes**, each taking the whole window, because a user works on a
config OR looks at its lanes, never both at once:

- `setup`: pick a config or make one. The toolbar holds the named configs
  stored on the hub (load one, New, Save under the typed name, Delete), the
  period in ms clamped to [1, 5000] (default 50), and Start. The window is
  two columns. Left, the node's measures as a tree folded on the `/` of
  their names (`estimator/attitude/pitch` is `pitch` under `attitude` under
  `estimator`; a folder that would hold one row is merged into its parent,
  so `rc/throttle` stays a leaf). Every folder has a tri-state box and a
  `ticked/total` count: ticking it selects everything under it, laid out one
  lane per unit (a group is ticked to be compared, and a shared y axis is
  only honest between measures that read alike), a member already selected
  lending its lane to the newcomers of its unit. A leaf ticked alone opens
  in a lane of its own, in the first unused hue. The filter box keeps the
  matching measures with their folders open; the folders left open are
  remembered per browser. Right, the lanes in order with one draggable chip
  per series: drop a chip on another lane to group them - only among
  measures of the same unit, the lanes that cannot take it grey out while
  the chip is in the air - drop it on the "new lane" target to split it out
  again, drag a lane by its grip to reorder. Nothing is rebuilt while a
  chip is in the air: removing the dragged element during its own dragstart
  cancels the drag.
- `live`: the lanes of that config, full height, with what is recorded in
  them. Record sends one `TelemetryCommand.config` (the ids and the period)
  and one `TelemetryCommand.subscribe` to the gateway, which configures the
  node and holds its stream while at least one client asks for it. Stop
  gives the subscription back once and leaves the curves on screen; so does
  a node switch, Edit config (back to setup) and `pagehide` /
  `beforeunload`, and a tab that dies takes its mark with its connection.
  Follow and pause behave as they always did. The toolbar names the config,
  its series count and the period the node applied, which comes back as
  `NodeTelemetryConfig`.

**Identity is the name.** A descriptor id is an index into the node's frozen
table and a reboot hands the same number to another measure, so a series is
bound by name and only ever routed by id. When the node's table changes the
routes are rebuilt from it: a measure that is gone is flagged absent and fed
nothing, and one whose name is back is rebound to whatever id it now has -
with the enable re-sent when a recording is running. A node that leaves the
table, or goes silent for three seconds while recording, pushes an explicit
`null` gap marker so the lanes break instead of drawing a chord across the
hole.

**View configs** are what is ticked and how it is laid out, with the
period and no data in them. Named ones live on the hub
(`/api/telemetry/configs`, see `software/hub/README.md`), so a config
outlives the browser that made it. The working config - what is ticked
right now - and the name it was loaded from are auto-saved to
`localStorage` (`mark4.pages.telemetry.v1`, `.name.v1`), debounced, and
restored on load. A config names measures, so it is applied against the
source node's table when that table is there: one loaded before the
gateway published the table waits for it, and a measure the node does not
expose is dropped rather than kept as a curve nothing can fill.

**CSV export** is long format (`series,unit,t_s,value`, gap markers
skipped), built here and stored under `/api/telemetry/exports` as
`<config>-<yyyymmdd>-<hhmmss>.csv`; the page then offers the link the hub
serves it from. There is no stored recording beyond that: what a recording
leaves behind is its export.

## The transport page

What the wire itself is doing, which is the one subject that is not about a
drone: every node's own view of the transport, gathered by the gateway (see
`software/hub/README.md`). The page paints what the gateway publishes and
derives nothing: `TransportHealth` for the verdicts, one `NodeTransport`
per node for the numbers.

**The reports are asked for.** A node streams its report only to whoever
subscribed, and the gateway subscribes only while a client asks, so the
page sends `TransportCommand { subscribe: true }` on every socket open and
`false` on `pagehide`. The `live reports` checkbox of the toolbar, checked
by default, sends the same command; clearing it also drops what was held,
so nothing stale stays on screen. With no client asking, the gateway's own
view is the whole picture and the wire pays nothing.

**The banner** is the system verdict, how many nodes report out of how many
are known, the worst edge and the frames per second over every edge held.
Under it the findings, worst first: one line per edge that loses frames or
has gone quiet, per asymmetric pair, per link that refused frames or could
not read them, per node that gave up on requests or saw its peers churn,
and one per node that does not report at all. The three thresholds
(`LOSS_DEGRADED` 1 %, `LOSS_BAD` 10 %, `FADING_MS` 1500) are repeated in
`src/transport/health.ts` from `software/hub/include/hub/transport_health.hpp`:
the gateway decides, the page colors and words what it decided.

**The graph** reads its topology from the reports. For every reporting node
and every link it has, a medium holds that node plus every peer it hears
over that link without a relay; two instances of the same medium kind that
share a member are the same wire seen from two ends, so they merge. The
columns are then fixed, left to right: the nodes on serial lines only, one
group per line, their buses, the nodes on both a serial line and the LAN
(the relay), the LAN buses, the nodes on the LAN only, and last the nodes
no report places, labelled with the hops the gateway sees them at. A column
nothing falls into takes no width. Geometry is fixed (240 px columns,
200 px cards 96 px high, 16 px between rows), so the picture of one bench
does not move between two reports.

An edge goes from the observer to the peer, because a loss belongs to the
direction that measured it: the losses towards the board are only ever
counted by the board. Edges above the degraded threshold are always drawn;
the others appear with the card hovered or selected. A dashed edge is one
whose other end does not report, so only half of that wire is known.

**The panel** is the selected node: its links (with the serial utilization
against 921600 baud), its peers, its messenger counters cumulative and over
the window, and two sparklines of the last minute - frames in per second
and worst in loss. They are two charts rather than one because frames and a
percentage share no scale. A node that does not report has none of that, so
its panel lists what the others hear of it instead.

**The theme.** The editor writes its colors as `--vscode-*` properties on
the root element of its webview and names the theme as a class on its body;
an iframe on another origin inherits neither, so the webview host posts
both as one `mark4-theme` message and `src/shared/theme.ts` (installed by
the shell, on every page) writes them where the page and the components
read them. A browser tab never gets that message: `transport.css` defines
the dark value of every variable the page and its components read on
`:root`, which is the only place in the pages where a color is written
out, and remaps the shared tokens of `style.css` onto them so the top bar
follows the theme too.
