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
# Configure + build + test (desktop, gcc). The CMake project root is
# software/: every cmake/ctest --preset command runs from there.
(cd software && cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop)

# Build one app by name (an app = a (cmake preset, target) pair in software/apps.json;
# also the "build app" VS Code task)
python3 scripts/build_app.py drone_sim

# Same under ASan/UBSan
(cd software && cmake --preset desktop-san && cmake --build --preset desktop-san && ctest --preset desktop-san)

# Cross-compile for STM32F405: the bootloader plus one firmware image per OTA
# slot, then the packaged bundle (software/build/stm32/drone_boot/drone_boot.elf,
# drone_firmware/drone_firmware_{a,b}.elf, slot_{a,b}.img, drone_firmware.ota)
(cd software && cmake --preset stm32 && cmake --build --preset stm32)

# Flash the board over SWD with a J-Link (never run by an agent, board required)
./scripts/flash_stm32.sh install   # also: boot | slot-a | slot-b | meta-wipe | erase

# Run one test (Catch2, by test name or ctest regex)
./software/build/desktop/tests/unit/unit_tests "kill switch forces all motors to zero"
(cd software && ctest --preset desktop -R "kill switch")

# Run the flight process (no frame limit by default; a finite budget is the
# DRONE_SIM_FRAME_LIMIT cmake cache variable, never a runtime argument)
./software/build/desktop/drone_sim/drone_sim [--sim-port N] [--discovery-port N] [--node-id N]

# Start a bench session: the hub takes no arguments, serves the pages and
# websocket on http://127.0.0.1:47810 and stays up; everything operational
# (board link, tuning profiles) is driven from the pages.
# Godot (own terminal or the "godot sim" VS Code task) and drone_sim are
# started and restarted by hand, discovery picks them up.
./software/build/desktop/hub/hub
godot --path sim-godot
./software/build/desktop/drone_sim/drone_sim

# Web pages (TypeScript, pnpm via corepack; the hub serves
# software/hub/pages/dist). Also: watch / typecheck / test
cd software/hub/pages && pnpm install --frozen-lockfile && pnpm build

# Editor extension (Mark4 sidebar + hub pages as webviews; local .vsix, no
# marketplace, no CI job). Install: "Extensions: Install from VSIX".
cd tools/vscode-mark4 && pnpm install --frozen-lockfile && pnpm build && pnpm package

# Monte Carlo throw campaign through headless Godot (see tools/batch/README.md)
python3 tools/batch/run_batch.py --runs 100 --parallel 4 [--godot /path/to/godot4]

# Golden packet decode tests (the ctest suite already checks the C++ side;
# CI runs all three in the desktop job)
python3 software/tests/golden/test_golden.py
godot --path sim-godot --headless --script res://tests/golden_check.gd -- "$(pwd)/software/tests/golden/fixtures"

# Lint (all must be clean before committing; CI runs exactly these)
git ls-files '*.cpp' '*.hpp' '*.c' '*.h' | xargs clang-format --dry-run --Werror
run-clang-tidy -p software/build/desktop -quiet "$(pwd)/software/(components|drone_sim|drone_firmware|hub|tests)/"
./scripts/tidy_stm32.sh    # clang-tidy over the stm32 compile database
./scripts/check_ascii.sh   # ASCII-only hard rule
```

clang-format and clang-tidy are pinned to LLVM 21 (devcontainer). Fix
formatting with `clang-format -i`. `software/tests/.clang-tidy` inherits the root
config and only relaxes magic numbers.

## Architecture

Everything C++ lives under `software/`: the executables at its top level
(`drone_sim`, `drone_firmware`, `hub`), the libraries in
`software/components/`. Four libraries, one rule of dependency flow:

- `flight-core/` - pure static lib. Single entry point
  `FlightCore::step(const SensorFrame&, ActuatorFrame&)`: synchronous,
  single-threaded, paced by data arrival, never by time (the timestamp
  travels inside the SensorFrame, stamped by platform at acquisition; the
  core never reads a clock). The RC kill switch is a frame field handled
  first in step(). `flight_core_types` is a separate INTERFACE target so
  platform headers can use SensorFrame/ActuatorFrame without a cycle.
  flight_core links flight_core_types alone: no platform, no protocol/
  (the telemetry packer is an IO adapter and lives in `platform_common`).
- `platform/` - 5 abstract services in `software/components/platform/include/platform/`
  (AbsSensorSource, AbsMotorSink, AbsCommandReceiver, AbsTelemetrySender,
  AbsClock). `AbsSensorSource::waitFrame()` is the single wait
  point of the whole system; AbsClock is internal to platform and never
  passed to FlightCore. Implementations live in `software/components/platform/src/<variant>/`
  (sim, stm32); each variant's headers stay under its own
  `src/<variant>/include/`. The `platform` INTERFACE target (headers only)
  carries the interfaces; `platform_common` (header-only) holds the
  composed helpers shared by every variant (TelemetryPublisher,
  packTelemetry); impl libs (`platform_sim`, `platform_stm32`)
  are declared only in the presets where they make sense (the
  DRONE_PLATFORM switch in `software/components/platform/CMakeLists.txt` and
  `software/CMakeLists.txt`) and are linked by the apps.
- `protocol/` - header-only packed wire structs. Every packet opens with
  a version byte then a type byte (nothing is demuxed by size); streams
  (telemetry, sim raw) add sourceId + u16 sequence; sizes, field offsets
  and little-endianness are static_asserted. The wire has exactly one
  copy per language: the C++ headers (source of truth),
  `tools/telemetry_wire.py` (all python tools import it)
  and `sim-godot/scripts/protocol.gd` (all Godot scripts read it), both
  guarded by the golden fixtures in `software/tests/golden/fixtures/` that CI
  decodes in all three languages. External processes (Godot sim, hub,
  ESP32 bridge) speak ONLY protocol/ and never link flight-core; the web
  pages (`software/hub/pages/`) speak only the hub's JSON over
  WebSocket/HTTP and never the wire. protocol/ is payload only: it names
  no address and no route.
- `transport/` - static lib, the interface manager between processes and
  boards (`software/components/transport/README.md`). Frames an opaque
  payload with `src u32, dst u32, seq u16, hops u8`, learns every node from
  any frame heard, beacons once per second, expires silent nodes, relays
  between links when asked. Depends on nothing but `drone_warnings`
  (`transport/serial_framing.hpp` lives here, the one CRC-16 of the
  project). The core and `UartLink` build for stm32 (no heap, fixed
  tables, function pointer callbacks); `UdpLink` is POSIX only: one shared
  discovery port (47820) for broadcasts, one ephemeral data socket per
  node for unicasts. Node ids are self-assigned `uint32_t` (random on
  desktop, `hashNodeId()` of the MCU UID on a board), never configured.
  Adopted between drone_sim and the hub, and by the batch campaign; the
  firmware, the bootloader and the ESP32 bridge still speak bare
  protocol/ packets over the serial framing.

Each executable is flight-core plus one composition of platform services,
assembled in an App class (see `software/drone_sim/drone_sim_app.hpp`): services
as value members, declaration order = construction/init order (destruction
is automatically the reverse), dependencies injected by reference, reference
accessors named `accessXxx()`, `bool init()` where the first failure logs
and returns false. main() only parses arguments, builds the App, runs it.
Replicate this pattern for firmware as it grows real services.

STM32 specifics: generic Cortex-M4F build via
`software/cmake/toolchain-arm-none-eabi.cmake` (`-nostartfiles`, newlib-nano, nosys).
`software/components/platform/src/stm32/startup.c` owns the vector table, the VTOR
setup and FPU enable, and the `_init`/`_fini` stubs. Flash is dual bank: the
bootloader (`software/drone_boot/`) lives at 0x08000000 and the firmware is
linked once per OTA slot, so there is no image at the reset vector other than
the bootloader. Each executable gets its own linker script, generated from
`stm32f405_image.ld.in` by `drone_stm32_image_layout()` (the stm32 variant's
CMakeLists.txt) out of three numbers: flash origin, flash length, image header
size. `scripts/make_ota.py` stamps the image headers and emits the `.ota`
bundle. The CCM RAM is not DMA-capable: future DMA buffers (DShot, SPI) must be
placed outside CCM.

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
