# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a 5-inch drone thrown by hand, motors off: detect the throw,
predict the apex, spin up, recover attitude, hover. Everything is written
from scratch as a learning project. The full reference document and roadmap
live in `docs/plan-dev.md`; read it before starting any new milestone.

## Hard rules

- **English only** across the repo: code, comments, docs, commit messages.
- **ASCII only** (no em dashes, arrows, typographic quotes, math symbols);
  accented letters are tolerated in proper names only.
- **Never reference the plan or milestones** in code, comments or module
  READMEs. Only `docs/plan-dev.md` talks about milestones.
- **Conventional Commits** (https://www.conventionalcommits.org/en/v1.0.0/).
- Coding conventions: `docs/contributing/cpp-guidelines.md` plus the root
  `.clang-format` / `.clang-tidy` (source of truth, enforced by CI). Key
  points: `Abs` prefix for abstract classes, `m_camelCase` members,
  `camelCase` methods, `UPPER_SNAKE` constants, Doxygen `@` notation,
  trailing `///<` on fields, `TODO(tmagne)` for deferred work.
- Single project namespace: `mark4`. No per-module namespaces. Include paths
  keep their module prefix (`#include "platform/clock.hpp"`).
- No singletons, no `new`/`delete` in flight-core and platform, no iostream
  in flight-core, no exceptions/RTTI in flight-core and platform (enforced
  with `-fno-exceptions -fno-rtti` via the `drone_strict` target).
- `float` everywhere with the lowercase `f` suffix; `-Wdouble-promotion` is
  an error on all presets (desktop must behave like the single-precision
  FPU of the F405).

## Commands

```sh
# Configure + build + test (desktop, gcc)
cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop

# Build one app by name (an app = a (cmake preset, target) pair in apps.json;
# also the "build app" VS Code task)
python3 scripts/build_app.py drone_sim

# Same under ASan/UBSan
cmake --preset desktop-san && cmake --build --preset desktop-san && ctest --preset desktop-san

# Cross-compile for STM32F405 (produces build/stm32/apps/firmware/firmware.elf)
cmake --preset stm32 && cmake --build --preset stm32

# Run one test (Catch2, by test name or ctest regex)
./build/desktop/tests/unit/unit_tests "kill switch forces all motors to zero"
ctest --preset desktop -R "kill switch"

# Run the flight process (no frame limit by default; a finite budget is the
# DRONE_SIM_FRAME_LIMIT cmake cache variable, never a runtime argument)
./build/desktop/apps/drone_sim/drone_sim [--sim-port N] [--telemetry-port N]

# Start a bench session: the hub takes no arguments, serves the pages and
# websocket on http://127.0.0.1:47810 and stays up; everything operational
# (board UART, recording, replay, profiles) is driven from the pages.
# Godot (own terminal or the "godot sim" VS Code task) and drone_sim are
# started and restarted by hand, discovery picks them up.
./build/desktop/apps/hub/hub
godot --path sim-godot
./build/desktop/apps/drone_sim/drone_sim

# Web pages (TypeScript, pnpm via corepack; the hub serves
# apps/hub/pages/dist). Also: watch / typecheck / test
cd apps/hub/pages && pnpm install --frozen-lockfile && pnpm build

# Editor extension (Mark4 sidebar + hub pages as webviews; local .vsix, no
# marketplace, no CI job). Install: "Extensions: Install from VSIX".
cd tools/vscode-mark4 && pnpm install --frozen-lockfile && pnpm build && pnpm package

# Monte Carlo throw campaign through headless Godot (see tools/batch/README.md)
python3 tools/batch/run_batch.py --runs 100 --parallel 4 [--godot /path/to/godot4]

# Golden packet decode tests (the ctest suite already checks the C++ side;
# CI runs all three in the desktop job)
python3 tests/golden/test_golden.py
godot --path sim-godot --headless --script res://tests/golden_check.gd -- "$(pwd)/tests/golden/fixtures"

# Lint (all must be clean before committing; CI runs exactly these)
git ls-files '*.cpp' '*.hpp' '*.c' '*.h' | xargs clang-format --dry-run --Werror
run-clang-tidy -p build/desktop -quiet "$(pwd)/(apps|flight-core|platform|protocol|tests)/"
./scripts/tidy_stm32.sh    # clang-tidy over the stm32 compile database
./scripts/check_ascii.sh   # ASCII-only hard rule
```

clang-format and clang-tidy are pinned to LLVM 21 (devcontainer). Fix
formatting with `clang-format -i`. `tests/.clang-tidy` inherits the root
config and only relaxes magic numbers.

## Architecture

Three libraries, one rule of dependency flow:

- `flight-core/` - pure static lib. Single entry point
  `FlightCore::step(const SensorFrame&, ActuatorFrame&)`: synchronous,
  single-threaded, paced by data arrival, never by time (the timestamp
  travels inside the SensorFrame, stamped by platform at acquisition; the
  core never reads a clock). The RC kill switch is a frame field handled
  first in step(). `flight_core_types` is a separate INTERFACE target so
  platform headers can use SensorFrame/ActuatorFrame without a cycle.
  flight_core links flight_core_types alone: no platform, no protocol/
  (the telemetry packer and the Blackbox recorder are IO adapters and
  live in `platform_common`).
- `platform/` - 6 abstract services in `platform/include/platform/`
  (AbsSensorSource, AbsMotorSink, AbsCommandReceiver, AbsTelemetrySender,
  AbsLogSink, AbsClock). `AbsSensorSource::waitFrame()` is the single wait
  point of the whole system; AbsClock is internal to platform and never
  passed to FlightCore. Implementations live in `platform/src/<variant>/`
  (sim, stm32, replay later); each variant's headers stay under its own
  `src/<variant>/include/`. The `platform` INTERFACE target (headers only)
  carries the interfaces; `platform_common` (header-only) holds the
  composed helpers shared by every variant (TelemetryPublisher,
  packTelemetry, Blackbox); impl libs (`platform_sim`, `platform_stm32`)
  are declared only in the presets where they make sense (the
  DRONE_PLATFORM switch in `platform/CMakeLists.txt` and
  `apps/CMakeLists.txt`) and are linked by the apps.
- `protocol/` - header-only packed wire structs. Every packet opens with
  a version byte then a type byte (nothing is demuxed by size); streams
  (telemetry, sim raw) add sourceId + u16 sequence; sizes, field offsets
  and little-endianness are static_asserted. The wire has exactly one
  copy per language: the C++ headers (source of truth),
  `tools/telemetry_wire.py` (all python tools import it)
  and `sim-godot/scripts/protocol.gd` (all Godot scripts read it), both
  guarded by the golden fixtures in `tests/golden/fixtures/` that CI
  decodes in all three languages. External processes (Godot sim, hub,
  ESP32 bridge) speak ONLY protocol/ over UDP and never link
  flight-core; the web pages (`apps/hub/pages/`) speak only the hub's
  JSON over WebSocket/HTTP and never the wire.

Each executable is flight-core plus one composition of platform services,
assembled in an App class (see `apps/drone_sim/drone_sim_app.hpp`): services
as value members, declaration order = construction/init order (destruction
is automatically the reverse), dependencies injected by reference, reference
accessors named `accessXxx()`, `bool init()` where the first failure logs
and returns false. main() only parses arguments, builds the App, runs it.
Replicate this pattern for firmware and drone_replay when they grow real
services.

STM32 specifics: generic Cortex-M4F build via
`cmake/toolchain-arm-none-eabi.cmake` (`-nostartfiles`, newlib-nano, nosys).
`platform/src/stm32/startup.c` owns the vector table, FPU enable and
`_init`/`_fini` stubs; `stm32f405.ld` propagates to executables through
INTERFACE link options on `platform_stm32`. The CCM RAM is not DMA-capable:
future DMA buffers (DShot, SPI) must be placed outside CCM.

Warnings come from the `drone_warnings` INTERFACE target (linked PRIVATE by
every project target, not by FetchContent deps such as Catch2).

## CI

`.github/workflows/devcontainer-image.yml` rebuilds the dev image and pushes
it to GHCR (`ghcr.io/keyrim/drone-mark4-devcontainer`) when `.devcontainer/`
changes; `ci.yml` runs 6 jobs (desktop+tests+golden decode in python and
GDScript, stm32, desktop-san, pages pnpm typecheck+build+test,
format+ascii, tidy desktop+stm32) inside that image, pinned by digest.
After a `.devcontainer/` change, bump the digest in `ci.yml` to the one the
image workflow pushed (`docker manifest inspect ...:latest`). Container jobs need `options: --user root` (the
image defaults to user `dev`, the runner mounts workdirs for another UID).
If CI needs a new tool, add it to the Dockerfile; the image is the single
source of truth for the environment (same versions as the devcontainer).
