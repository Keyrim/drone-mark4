# Git worktrees

This directory is where `git worktree` checkouts of this repository live, so
that several branches can be worked on in parallel without juggling one
checkout. Everything here except this README is gitignored, so the checkouts
never show up as untracked content.

A worktree is usable for the C++ build straight after `git worktree add`,
but it has the tracked files only: no generated codecs, no pnpm or Flutter
dependencies. The bring-up below says what each area needs. Read the whole
page before creating your first one.

## Create

Always from **inside the dev container**, from the repository root:

```bash
git worktree add worktrees/my-feature -b feat/my-feature
```

Two constraints, both of which silently produce a broken worktree if ignored:

- **Inside the container, not on the host.** The container's system git
  config sets `worktree.useRelativePaths` (see the git layer of
  [`../.devcontainer/Dockerfile`](../.devcontainer/Dockerfile)), so the two
  links between the worktree and the main clone (`worktrees/my-feature/.git`
  and `.git/worktrees/my-feature/gitdir`) are relative and resolve both at
  the container path (`/workspaces/drone-mark4`) and at the host's own clone
  path, which is different. This needs git >= 2.48 (noble ships 2.43, hence
  the PPA in the Dockerfile). A host git that writes absolute paths produces
  links that do not resolve in the container, and the other way round.
- **Under `worktrees/`, nowhere else.** Only the clone is bind-mounted into
  the container. A worktree created outside it (`../my-feature`, `/tmp/...`)
  exists solely in the container's writable layer and is lost when the
  container is rebuilt.

The first relative worktree changes the main clone itself: git writes
`extensions.relativeWorktrees = true` to `.git/config` and bumps
`core.repositoryformatversion` from 0 to 1. From then on a git older than
2.48 refuses every command on the whole clone, main checkout included:

```
fatal: unknown repository extension found:
	relativeworktrees
```

The host git is recent enough. A container started from an image whose git
predates the PPA layer is not, and has to be rebuilt before it can run any
git command here.

## Bring up

`git worktree add` checks out tracked files only. Everything gitignored is
absent: generated codecs, package installs, build trees. Nothing is mandatory
for everyone; run the step of the area you touch.

- **C++ (flight-core, platform, drone_sim, hub, firmware)**: nothing.
  `cmake --preset desktop` configures a fresh build tree under
  `software/build/desktop`, fetches the FetchContent dependencies (Catch2,
  nanopb, ...) again for it and generates the nanopb codec in it. The
  desktop build also writes `sim-godot/scripts/gen/` (godobuf, target
  `proto_gd`) and `software/build/desktop/gen/python`.
- **Tests**: `ctest --preset desktop` spawns a headless Godot for the two
  GDScript transport checks, and Godot resolves the plant's `class_name`
  globals from its import cache, `sim-godot/.godot/`, which a fresh worktree
  does not have. Without it exactly those two tests fail with
  `Identifier "Mark4Transport" not declared in the current scope`. Import
  the project once after the desktop build (the codec must exist first), as
  CI does: `godot --path sim-godot --headless --import`.
- **Hub pages** (`software/hub/pages`): `pnpm install --frozen-lockfile`,
  then any pnpm script; each starts with `pnpm gen`, which writes `src/gen/`.
  `node_modules/`, `dist/` and `src/gen/` are all gitignored, so until then
  every import of a generated codec resolves to nothing.
- **Editor extension** (`tools/vscode-mark4`): same shape, `pnpm install
  --frozen-lockfile` then `pnpm build` (`node_modules/`, `dist/`, `src/gen/`
  and the `.vsix` are gitignored).
- **Mobile app** (`software/mobile`): `flutter pub get && ./tool/gen.sh`
  writes `.dart_tool/` and `lib/gen/` (Dart codec, wire hash, ffigen
  binding). Without it `flutter analyze` reports a `uri_does_not_exist`
  error for every generated import and says nothing about your changes.
- **Godot plant** (`sim-godot`): the codec comes from the desktop build
  above, the import cache from the same `--import` run, or from the editor
  on first open.

```bash
cd worktrees/my-feature
(cd software && cmake --preset desktop && cmake --build --preset desktop)   # C++ and the Godot codec
godot --path sim-godot --headless --import                                    # Godot import cache, for ctest and the plant
(cd software/hub/pages && pnpm install --frozen-lockfile && pnpm build)      # hub pages
(cd tools/vscode-mark4 && pnpm install --frozen-lockfile && pnpm build)      # editor extension
(cd software/mobile && flutter pub get && ./tool/gen.sh)                     # mobile app
```

`logs/`, `software/compile_commands.json` and `.pnpm-store/` are also absent
and come back on their own (the hub, the CMake Tools copy, pnpm).

## Build

Nothing special: the build commands in [`../CLAUDE.md`](../CLAUDE.md) work
unchanged from the worktree root. CMake's `binaryDir` is
`${sourceDir}/build/${presetName}`, so each worktree gets its own build
trees under `software/build/` and none of them collide with the main
clone's.

Budget the disk, though. A fresh worktree with the `desktop` preset built
and tested, the Godot project imported and the hub pages installed measures
761 MB (`du -sh`), of which the build tree is about 650 MB and the pages'
`node_modules` about 110 MB. Each further preset adds its own tree
(`desktop-san` is the largest, around 780 MB; `stm32` is small), and the
extension's `node_modules` weighs about as much as the pages'.

## Open in an editor

Open a worktree in its own VS Code window: File > Open Workspace from File,
pointing at the worktree's own copy of `drone-mark4.code-workspace`, from
within the existing container window. Its folder paths are relative, so it
works as is. Do not go through "Reopen in Container" on the host for a
worktree: it would offer to build a second container for it.

`worktrees/` is excluded from the file watcher, search and the Dart
analyzer in [`../drone-mark4.code-workspace`](../drone-mark4.code-workspace),
so the main window does not index the checkouts; each is a complete C++ +
TypeScript + Flutter tree and indexing them all multiplies the workload.

Two `folderOpen` tasks of the tracked `.vscode/tasks.json` run when a
worktree window opens, exactly as they do for the main clone:

- **`install mark4 extension`** (`tools/vscode-mark4/install.sh`) installs
  the worktree's extension when its `package.json` version differs from the
  installed one. The install is editor-wide: a worktree with a bumped version
  replaces the extension for every window, the main clone's included.
- **`start hub`** (`scripts/start_hub.sh`) rebuilds the worktree's pages,
  then exits without starting anything if a hub already answers on
  `http://127.0.0.1:47810`. With the main clone's hub running, the worktree's
  hub code is therefore not what serves. Stop the main hub first to test hub
  changes from a worktree.

There is one bench, whatever checkout the binaries came from: every process
is a transport node on `udp/47820` and the container runs on the host
network, so a `drone_sim` started from a worktree is heard by whichever hub
and Godot plant are running.

## Remove

Remove a worktree with git rather than `rm -rf`, so its administrative entry
under `.git/worktrees/` goes too:

```bash
git worktree remove worktrees/my-feature
git branch -D feat/my-feature
```

The plain form works with the build trees and `node_modules` in place:
ignored files are not a reason for git to refuse. It does refuse when the
worktree has uncommitted changes or untracked files that are not ignored;
`git worktree remove --force` then discards them without asking, so check
`git status` in the worktree first.

The branch outlives the worktree; delete it separately once its work is
merged or abandoned.

If a worktree directory was deleted by hand, `git worktree prune` drops the
leftover administrative entries.

## Claude Code

Claude Code's built-in worktree feature defaults to `.claude/worktrees/`, not
here. That path is gitignored so a stray checkout does no harm, but the
convention for this repository is `worktrees/` and the bring-up above
applies either way.
