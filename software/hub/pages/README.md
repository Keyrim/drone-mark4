# Hub pages

The web front end the hub serves: two windows meant for two screens,
`index.html` (control: commands and the 3D attitude, the default view) and
`plots.html` (the lanes). Plain TypeScript bundled by esbuild into `dist/`, one
ESM bundle per page plus a single `style.css`: no framework, no runtime
template engine, nothing for the hub to do beyond handing out static files.

```sh
pnpm install
pnpm build       # dist/, minified, what the hub serves
pnpm watch       # rebuild on change, with sourcemaps
pnpm typecheck
pnpm test
```

## Layout

- `src/shared/` - modules every page uses: the websocket link
  (`hub_socket.ts`), the page shell (`shell.ts`, top bar + discovery +
  toasts), the quaternion helpers (`quat.ts`) and the series catalog
  (`series.ts`, including the one-color-per-drone map).
- `src/lanes/` - the lane viewer: time-axis math, sample buffers, the uPlot
  charts, the ruler and the lane configuration panel.
- `src/console/` - the control window: one widget per connected drone
  (`drone_widget.ts`), whatever its nature. Observation is the same for
  every nature (phase, throw detector, altitude, motors); the controls
  depend on it - kill/arm/mode switches and a throttle slider for a real
  or simulated drone, replay controls for a blackbox re-execution, plus
  the folded scenario and tuning blocks. The "Add drone" block holds the
  two manual doors (board UART, blackbox); UDP drones appear on their
  own. All drones draw superimposed in the 3D view, one color each. `rc.ts` is the piloting state machine,
  pure and unit tested. `ota_panel.ts` is the firmware update panel, over
  the pure `ota.ts`.
- `src/<page>/main.ts` - one page. Every directory holding a `main.ts`
  becomes its own bundle, so adding a page is adding a directory and an
  `.html` file next to `esbuild.js`.
- `test/` - node:test suites run through tsx.

## Talking to the hub

The pages are served by the hub, so the websocket URL is always
`ws://<the host that served this page>`. There is nothing to configure.

Messages are JSON objects carrying a `type` field; handlers are registered
per type and an unknown type is ignored, so a hub that learns a new message
never breaks an older page.

Acks are broadcast to every connected client, so a correlation id has to say
which tab asked. Each tab draws a random nonce at load and numbers its
requests inside that block (`id = nonce * 65536 + counter`); an ack whose id
is not in the map belongs to another tab and is dropped.

## Exact state alignment (the latch rule)

The estimated state arrives in `telemetry` messages, the exact simulator
state in `simRaw` messages. They are two independent streams with their own
timestamps, but a lane is a single x axis, so the two have to share one.

A `simRaw` message therefore **never pushes a row of its own**. It only
latches its values, and the latch is read when the next `telemetry` row
lands, at the telemetry timestamp. This is the causal form of the hub's 30 ms
alignment rule: a page can only ever look backwards, so it pairs each
telemetry row with the last exact state it has seen.

Two consequences worth knowing:

- The error angle a page shows in live mode is computed page-side, from that
  causal pairing, in both live and replay. It is a readout, not a score.
- The aggregate score stays hub-side. `GET /api/compare` aligns both streams
  offline, with the future available, and its numbers are the authority. A
  disagreement between the two is expected, not a bug.

## Replay

Replay mode reads the recordings the hub already holds, over its REST API:
`/api/recordings` to list them, `/api/recording` to open one into the same
lanes, `/api/summary` and `/api/compare` for the cards, `/api/file` for the
raw downloads. `?rec=NAME` opens a recording straight away and the page keeps
that link up to date, so a run is shareable by URL.

A streams recording replays through the latch rule above, so a value read in
replay is the value that was on screen live. A blackbox recording has no
estimate and no exact state: its series are its columns, and its lanes are
built from the header the hub sent rather than from the catalog.

## Piloting

The transmitter of a drone widget is its switches (kill, arm, mode) and its
throttle slider: no engage ritual, no keyboard layer. The widget streams the
RC state at 10 Hz from the first interaction on and never stops while the
page is visible.

**The silence is the fail-safe**: the drone cuts on its own RC timeout, so a
closed tab, a frozen browser or a dead link all end the same way. Hiding the
page flips the kill switch on - a pilot who cannot see the drone is not
piloting it - and a widget leaving (its drone disappeared) sends the safe
state twice before its stream stops. The hub counts the clients that
streamed RC recently and the top bar warns when there is more than one.

`rc.ts` keeps the pure part (safe state, clamping, the exact payload) under
`test/rc.test.ts`.

## Firmware update

The update panel sits under the drone list on the control page, below the
widgets and above "Add drone": reflashing is an operation on the drone that is
already there, and the panel has to stay readable while the board reboots and
its widget momentarily disappears.

It shows the two identities side by side (what the board runs, from which
slot, against what the bundle holds), the bundle path prefilled with the
build output the hub defaults to, the phase in plain words, a progress bar
following the bytes the board acknowledged writing, and the verdict sentence
the hub wrote. The auto-confirm switch, the abort, the manual confirm and the
revert are the four gestures; each is offered only where it would do
something, and none is offered without a board link.

The hub owns the whole state machine and publishes it as one `ota` message on
every change, so the panel derives nothing: it paints the last message. While
nothing is running it asks for a fresh board status every three seconds, since
the version of a board that was just plugged in is a question, not an event.

`ota.ts` keeps the pure part (decoding the message, the phase words, and the
rule that says which buttons make sense) under `test/ota.test.ts`.

## View configs

The lane layout is a *view*: a name, and which series each lane draws. Views
live in `localStorage` under `mark4.pages.viewConfigs`. They record nothing
and own no data. Recording is the hub's business and the blackbox is the
session, which is why the pages know exactly two modes: `live` and `replay`.
