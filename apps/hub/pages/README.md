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

## View configs

The lane layout is a *view*: a name, and which series each lane draws. Views
live in `localStorage` under `mark4.pages.viewConfigs`. They record nothing
and own no data. Recording is the hub's business and the blackbox is the
session, which is why the pages know exactly two modes: `live` and `replay`.
