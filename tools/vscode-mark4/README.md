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
  start action.
- **Log levels**: every module of every node with its current threshold,
  grouped by node or by module name (the title action switches). A `/` in
  the names makes a folder (`platform/`) when two modules or more share it.
  "Set level..." on a leaf, a folder or a node sends one `LogControl.set` per
  module below it and then a query, so the tree shows what the nodes
  confirmed rather than what was asked.
- **Bench**: what is not a node, the two pages to dock. `Mark4: Bench
  Session` (rocket icon) builds and starts the hub, starts the Godot sim,
  then docks the control and plots pages in two editor groups.

The **Mark4 Logs** output channel prints every `Log` envelope the gateway
forwards, one line, through the matching level so VS Code owns the filter and
the colors: `kind | node id | module | text`, in columns (a longer kind or
module is cut, `drone_sim` reads `drone_si`). The kind and the module name
come from the node table; an unknown module prints as `#id`. The gateway's
own lines arrive as frames from its node id like every other node's. Nothing
is buffered: a reconnection writes one line and the stream resumes.

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
  the log levels view resets with the window.
- The log channel keeps no history of its own; VS Code's output view is the
  scrollback.
- The nodes view refreshes on the gateway's table (once a second), so the
  live/fading dot is that old at worst.
