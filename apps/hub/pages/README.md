# Hub pages

The web front end the hub serves. Plain TypeScript bundled by esbuild into
`dist/`, one ESM bundle per page plus a single `style.css`: no framework, no
runtime template engine, nothing for the hub to do beyond handing out static
files.

```sh
pnpm install
pnpm build       # dist/, minified, what the hub serves
pnpm watch       # rebuild on change, with sourcemaps
pnpm typecheck
pnpm test
```

## Layout

- `src/shared/` - modules every page uses: the websocket link
  (`hub_socket.ts`), the page shell (`shell.ts`), the quaternion helpers
  (`quat.ts`) and the series catalog (`series.ts`).
- `src/lanes/` - the lane viewer: time-axis math, sample buffers, the uPlot
  charts, the ruler and the lane configuration panel.
- `src/console/` - the console: target selection, state readout, scenarios,
  recording, tuning and profiles, replaying a recording, and keyboard
  piloting. `rc.ts` is the piloting state machine, pure and unit tested.
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

## Keyboard piloting

The console can stream RC from the keyboard, and the rules around it are the
feature: a browser tab is not a transmitter, so the design assumes it will
stop paying attention at the worst moment.

There is exactly one RC state, mutated by the buttons and the keys alike,
streamed at 10 Hz and only while the pilot toggle is engaged. While it is
off, the kill, arm and mode buttons are disabled - a one-shot arm would die
at the drone's own RC timeout anyway, and that timeout is the whole point.

`K` toggles the kill, `Space` is a panic kill that can never un-kill, `Esc`
disengages, `A` toggles the arm, `M` toggles the mode, the arrows ramp the
throttle at 40 percent per second (`Shift` for 8), `0` or `Home` zeroes it
and `C` centers it at 50 percent, which is what altitude-auto arming wants.
Auto-repeat is ignored: the ramp is applied at the stream tick, not at the
key event, so a held key ramps at a known rate.

Disengaging resets the state to kill-engaged, disarmed, stick down; the page
sends that twice and then stops streaming. **The stop is the fail-safe**: the
drone cuts on its own RC timeout, so a frozen browser is covered by the same
mechanism as a clean exit. It happens on the toggle, `Esc`, the window losing
focus, the tab going to the background, the page unloading, the hub link
dropping, a tick more than 300 ms late, and a 60 s deadman.

While engaged a red banner shows the measured stream rate, the commanded
throttle against the motor outputs telemetry reports, and a warning when the
hub has more than one client - another tab could be streaming RC too.

`rc.ts` holds all of that as a pure `(state, event) -> state` function, so
the rules are covered by `test/rc.test.ts` rather than by a click-through.

## View configs

The lane layout is a *view*: a name, and which series each lane draws. Views
live in `localStorage` under `mark4.pages.viewConfigs`. They record nothing
and own no data. Recording is the hub's business and the blackbox is the
session, which is why the pages know exactly two modes: `live` and `replay`.
