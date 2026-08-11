# vscode-mark4

Thin editor extension around the existing tooling. It adds a Mark4 sidebar
with two views and opens the hub pages inside the editor:

- Apps: one line per apps.json entry, with inline build / run / debug. Build
  and run shell out to `scripts/build_app.py` / `scripts/run_app.py` (the
  single build implementation); debug starts the launch.json configuration
  named after the app.
- Bench: hub state (an HTTP ping on its URL), godot sim state, and the two
  pages. `Mark4: Bench Session` (rocket icon) builds and starts the hub,
  starts the Godot sim, then docks the control and plots pages in two editor
  groups.

The pages render in webviews as full-frame iframes on
`http://127.0.0.1:47810`; the hub keeps serving them, so a browser tab still
works. The extension knows that URL and nothing else: no wire structs, no
JSON. If a download (recording export) misbehaves inside a webview, open the
page in a browser for that one action.

## Build and install

```sh
cd tools/vscode-mark4
pnpm install --frozen-lockfile
pnpm build
pnpm package        # produces vscode-mark4-<version>.vsix
```

Install with "Extensions: Install from VSIX" in the command palette. Repeat
after changes (`pnpm build && pnpm package`, then reinstall). `pnpm watch`
plus F5 (Extension Development Host) works for iterating.
