# vscode-mark4

Thin editor extension around the existing tooling. It adds a Mark4 sidebar
with four views, an output channel for the logs of the whole bench, and opens
the hub pages inside the editor. Everything about the running bench comes
from one websocket to the gateway (the hub, `ws://127.0.0.1:47810`): the
extension is one more client of `gateway.proto`, like the pages.

- **Apps**: one line per apps.json entry, with inline build / run / debug.
  Build and run shell out to `scripts/build_app.py` / `scripts/run_app.py`
  (the single build implementation); debug starts the workspace launch
  configuration named after the app.
- **Nodes**: one line per node of the gateway's `NodeTable`, whoever started
  it: an icon per kind (firmware, drone_sim, plant, gateway, batch, relay, a
  generic one for a kind this build does not know), the name, then the kind
  and the node id in 8 hex digits. Green while the node was heard less than
  1.5 s ago, grey ("fading") after that, and a warning icon plus `WIRE
  MISMATCH` when the node announces another `mark4.proto` hash than the
  gateway's. The tooltip has the address, the build date, the git and wire
  hashes and the frame counters. Inline stop for what runs on this machine
  (the hub, the Godot plant, a drone_sim the extension started); the title
  bar starts one more `drone_sim` (`+`) or the Godot sim (globe). With no
  gateway the view is one line, "gateway offline: start the hub", with a
  start action. The table arrives once a second and almost never differs:
  the view is only redrawn for the lines that read differently (a name, a
  kind, live or fading, a wire mismatch), and only the whole view when
  nodes appear or leave. The counters and the last-seen of the tooltip are
  not a reason to redraw, so they are one refresh behind at worst.
- **Log levels**: every module of every node with its current threshold,
  grouped by node or by module name (the title action switches). A `/` in
  the names makes a folder (`platform/`) when two modules or more share it.
  "Set level..." on a leaf, a folder or a node sends one `LogControl.set` per
  module below it and then a query, so the tree shows what the nodes
  confirmed rather than what was asked; the same gesture moves what the log
  channel shows of that same scope, so the lines follow the level. On a
  node, "Hide / Show in logs" (eye) drops its lines from the channel and
  touches nothing on the wire. The title bar has the same gesture over the
  whole bench, "Set level for everything..." (gear): every module of every
  node of the table at once. `Mark4: Reset Levels To INFO` (command palette)
  is that same change at INFO, the level a node starts at. A node that has
  not published its module table yet has no module to set: it is skipped and
  named in the `Mark4` channel. The title bar also searches the logs
  (magnifier, an empty answer clears the search) and clears them (the store,
  not the filter), and shows the channel (`Mark4: Show Logs`, `ctrl+alt+l` /
  `cmd+alt+l`, which keeps the focus where it is). Like the nodes view, it
  only redraws the subtrees that read differently.
- **Bench**: what is not a node, the two pages to dock. `Mark4: Bench
  Session` (rocket icon) builds and starts the hub, starts the Godot sim,
  then docks the control and plots pages in two editor groups.

The **Mark4 Logs** output channel separates what was received from what is
shown. Every `Log` envelope the gateway forwards is stored raw (a ring of
50000 records: the extension's own clock at reception, the node, the module
id, the level, the text) and nothing is formatted at ingest. The channel is
then the projection of that store, redrawn (at most five times a second)
whenever a node table, the display filter or the search changes; new lines
that pass are appended. The extension owns the whole line, so the columns
line up:

```
HH:MM:SS.mmm  LEVEL  kind       node id   module                    text
11:54:38.887  DEBUG  drone_sim  da532bb1  sim/link                  t=4.004 s, 2000 frames
```

Two spaces between fixed columns: the time to the millisecond, the level in
5 characters (`TRACE DEBUG INFO  WARN  ERROR`), the kind padded to 9 and
never cut, the node id in 8 hex digits, the module padded or cut to 24. The
kind and the module name are resolved at render time from the node table, so
a node announcing late names the lines it sent before: until then they read
`unknown` and `#id`, and the next redraw names them all. The gateway's own
lines arrive as frames from its node id like every other node's; the state of
the link itself has no node, so "reconnected to the gateway" is stored as a
line of a pseudo node `00000000` named `gateway link` and survives the
redraws like any other.

What is shown of the store is a minimum level per (node, module), INFO until
a "Set level..." says otherwise, plus a hidden flag per node and one text
search (case insensitive, over the whole rendered line: a kind, a node id or
a module name is searched like the text). Hence the one thing to know:
**lowering a level shows the stored lines at once, raising it only shows
what arrives from then on** - the node was not emitting them before, so
nothing was received to show. Nothing is persisted: closing the window
empties the store and resets the filter.

The channel is created with a language of its own (`mark4-log`, the grammar
`syntaxes/mark4-log.tmLanguage.json`), so the theme in use colors it. Color
codes the level and nothing else: one rule matches the whole line and gives
that whole line a single standard scope, picked by the level column. No
column has a scope of its own, so the kind, the node id and the module read
like the text. A line that does not hold the layout (a wrapped line, a kind
wider than its column) matches nothing and stays plain rather than being
colored wrong.

| Level | Scope | Dark+ | Light+ |
| --- | --- | --- | --- |
| `ERROR` | `invalid.illegal` | red | red |
| `WARN` | `string` | orange | dark red |
| `INFO` | none | default | default |
| `DEBUG` `TRACE` | `comment` | green grey | green |

INFO is the level a node runs at, so it is the one left plain: what stands
out is what left the normal course (WARN, ERROR) and what was asked for on
purpose (DEBUG and TRACE, dimmed).

Several `drone_sim` at once: the `+` action starts each instance with its own
emulated flash (`software/build/desktop/drone_sim/ota_flash_<n>`) and with a
node id the extension picks (`0xd500000<n>`), which is what ties the line of
the nodes view back to the task to stop. A `drone_sim` started from anywhere
else has a random id, so it is listed like any other node but offers no stop.

The pages render in webviews as full-frame iframes on
`http://127.0.0.1:47810`; the hub keeps serving them, so a browser tab still
works. If a download (recording export) misbehaves inside a webview, open the
page in a browser for that one action.

## Build and install

```sh
cd tools/vscode-mark4
pnpm install --frozen-lockfile
pnpm gen            # the TypeScript codecs, run by every script below
pnpm typecheck
pnpm test           # node:test over the pure logic
pnpm build
pnpm package        # produces vscode-mark4-<version>.vsix
pnpm smoke          # against a live bench, see scripts/smoke.ts
```

`pnpm gen` generates `src/gen/mark4_pb.ts` and `src/gen/gateway_pb.ts` from
the two schemas of `software/components/protocol/` with `protoc-gen-es`,
exactly as the hub pages do (same versions of `@bufbuild/protobuf` and
`@bufbuild/protoc-gen-es`, same `protoc` from the python `grpcio-tools`).
`src/gen/` is gitignored: nothing generated is committed, and the extension is
always built against the schema of the tree it sits in.

Install with "Extensions: Install from VSIX" in the command palette. Repeat
after changes (`pnpm build && pnpm package`, then reinstall). `pnpm watch`
plus F5 (Extension Development Host) works for iterating.

## Deliberate simplifications

- No settings and no persistence: the hub URL is a constant, the grouping of
  the log levels view, the display filter and the log store reset with the
  window.
- The display filter is keyed by (node, module) pair: a level set on a node
  or on a folder is written on every module below it, and a module that
  appears later starts at INFO like everything else.
- The search filters the rendered lines rather than the records, which is
  what makes it search the kind and the module name too. No regex, no case.
- The nodes view refreshes on the gateway's table (once a second), so the
  live/fading dot is that old at worst.
- The colors are a grammar over standard scopes, no theme and no semantic
  tokens: whatever theme is in use colors the lines, and none of the four
  scoped levels is guaranteed a color of its own.
- "Reset levels to INFO" has no title button: the gear already asks for a
  level, and INFO is one of the five it offers.
