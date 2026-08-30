# Hub pages

The web front end the hub serves: two windows meant for two screens,
`index.html` (control: one widget per drone node and the 3D attitude, the
default view) and `plots.html` (the lanes). They show the drones, not the
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
  own controls, the connection state and the toasts), the quaternion helpers (`quat.ts`) and the series catalog
  (`series.ts`).
- `src/lanes/` - the lane viewer: time-axis math, sample buffers, the uPlot
  charts, the ruler and the lane configuration panel.
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

The estimated state is the `Telemetry` envelope; the exact plant state,
when the sender has a plant, is its `truth` field, sampled at the same
instant. One message is one row of every series (`sampleTelemetry`), the
error angle a page shows is computed page-side from the two quaternions of
that row: a readout, not a score. The 3D ghost is the truth of whichever
live drone streams one.

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

## View configs

The lane layout is a *view*: a name, and which series each lane draws. Views
live in `localStorage` under `mark4.pages.viewConfigs`. They record nothing
and own no data.
