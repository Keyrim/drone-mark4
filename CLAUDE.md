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
- **No `printf` / `fprintf` / `rttWrite` for diagnostics**: every line goes
  through the log library with a module (`software/components/log/`,
  `static LogModule MODULE{id, "area/thing"}` then `MODULE.info(...)`).
  Only `main()`'s usage text on a bad argument stays a plain `fprintf`.
  Node ids print as 8 hex digits everywhere.

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
./software/build/desktop/drone_sim/drone_sim [--discovery-port N] [--node-id N]

# Start a bench session: no port to pass anywhere, every process is a
# transport node on udp/47820 and finds the others by their beacons. The hub
# takes no arguments, serves the pages and websocket on http://127.0.0.1:47810
# and stays up; everything operational (board link, tuning profiles) is driven
# from the pages. Godot (own terminal or the "godot sim" VS Code task) hosts
# one virtual drone per drone_sim it hears; drone_sim processes are started
# and restarted by hand, in any order, as many as wanted.
./software/build/desktop/hub/hub
godot --path sim-godot
./software/build/desktop/drone_sim/drone_sim

# Web pages (TypeScript, pnpm via corepack; the hub serves
# software/hub/pages/dist). Every script first runs `pnpm gen`, which
# generates the TypeScript codecs of mark4.proto and gateway.proto into
# src/gen/ (gitignored) with protoc-gen-es. Also: watch / typecheck / test /
# smoke (a ws client against a live bench)
cd software/hub/pages && pnpm install --frozen-lockfile && pnpm build

# Editor extension (Mark4 sidebar: apps, nodes, log levels, bench, plus the
# hub pages as webviews and a "Mark4 Logs" output channel; it is one more
# gateway.proto client and generates its own TypeScript codec with `pnpm
# gen`. Local .vsix, no marketplace, no CI job). Install: "Extensions:
# Install from VSIX".
cd tools/vscode-mark4 && pnpm install --frozen-lockfile && pnpm build && pnpm package

# Mobile app (Flutter, Android; software/mobile is the Flutter project root).
# The phone is reached over Wi-Fi: ./scripts/adb_wifi.sh discovers, pairs and
# connects it (wireless debugging on, same Wi-Fi). Never run flutter on the host.
cd software/mobile && flutter analyze && flutter build apk --debug --target-platform android-arm64

# Monte Carlo throw campaign through headless Godot (see tools/batch/README.md;
# needs the desktop build for drone_sim and the generated python codec)
python3 tools/batch/run_batch.py --runs 100 --parallel 4 [--godot /path/to/godot4]

# Wire codecs: generated from software/components/protocol/mark4.proto by the
# desktop build (nanopb C into the build tree, godobuf GDScript into
# sim-godot/scripts/gen/, python into software/build/desktop/gen/python).
# Regenerate = rebuild; the two targets alone:
(cd software && cmake --build --preset desktop --target proto_gd proto_py)

# Lint (all must be clean before committing; CI runs exactly these)
git ls-files '*.cpp' '*.hpp' '*.c' '*.h' | xargs clang-format --dry-run --Werror
# (clang-tidy reads the generated mark4.pb.h / gateway.pb.h: build the protocol
# and nanopb_gateway targets of the preset first, a full build does it)
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
`software/components/`. Six libraries, one rule of dependency flow:

- `flight-core/` - pure static lib. Single entry point
  `FlightCore::step(const SensorFrame&, ActuatorFrame&)`: synchronous,
  single-threaded, paced by data arrival, never by time (the timestamp
  travels inside the SensorFrame, stamped by platform at acquisition; the
  core never reads a clock). The RC kill switch is a frame field handled
  first in step(), sensor health right after: `SensorFrame::imuValid` /
  `baroValid` say whether the fields are fresh measurements for this frame
  (`software/components/flight-core/README.md`: no integration and no
  arming without an IMU, `FlightPhase::FAULT` when it is lost with the
  motors running, a lost baro coasts). `flight_core_types` is a separate INTERFACE target so
  platform headers can use SensorFrame/ActuatorFrame without a cycle.
  flight_core links flight_core_types and `telemetry`: no platform, no
  protocol/ (the status packer is an IO adapter and lives in
  `platform_common`). The telemetry link is names, units and pointers, not
  wire: a module declares what it computes as a `TelemetryEntry` next to
  the variable.
- `platform/` - 4 abstract services in `software/components/platform/include/platform/`
  (AbsSensorSource, AbsMotorSink, AbsCommandReceiver, AbsClock). There is
  no output service: everything a composition emits leaves through its
  `Transport` (`sendEnvelope(transport, dst, envelope)`).
  `AbsSensorSource::waitFrame()` is the single wait
  point of the whole system; AbsClock is internal to platform and never
  passed to FlightCore. Implementations live in `software/components/platform/src/<variant>/`
  (sim, stm32); each variant's headers stay under its own
  `src/<variant>/include/`. The `platform` INTERFACE target (headers only)
  carries the interfaces; `platform_common` (header-only) holds the
  composed helpers shared by every variant (TelemetryPublisher,
  packTelemetry); impl libs (`platform_sim`, `platform_stm32`)
  are declared only in the presets where they make sense (the
  DRONE_PLATFORM switch in `software/components/platform/CMakeLists.txt` and
  `software/CMakeLists.txt`) and are linked by the apps. A source always
  delivers frames at the nominal cadence and flags what it could not
  measure (`software/components/platform/README.md`); the sim variant owns
  the plant entirely: with one, the plant's SimSensor cadence paces the
  loop, without one the platform clock does and the frames carry no
  sensors, so `drone_sim` needs no plant to start
  (`software/components/platform/src/sim/README.md`).
- `protocol/` - one protobuf schema, `mark4.proto` (+ `mark4.options`
  nanopb bounds), codecs generated at build time and never committed:
  nanopb C for every C/C++ target (the `nanopb` lib, `PB_NO_MALLOC`,
  `PB_BUFFER_ONLY`; the `protocol` lib adds `encodeEnvelope()` /
  `decodeEnvelope()` in `protocol/envelope.hpp`), godobuf GDScript for
  Godot (`sim-godot/scripts/gen/`, target `proto_gd`), `protoc
  --python_out` for the batch tool (target `proto_py`). Two different
  things travel on it: `Status`, the small fixed report of what the drone
  is doing (attitude, motors, phase, throw state and count, the validity
  flags, the plant truth when there is one), broadcast every 10 frames and
  always on; and the telemetry family (`TelemetryListRequest`/`Descriptors`,
  `TelemetryEnable`/`Ack`, `TelemetryData`), unicast on demand, one active
  stream per drone, which is how any registered measure is plotted without
  touching the schema. Every message on
  every link is one `Envelope` (a oneof over all messages); there is no
  version byte, a 32-bit hash of the schema (`WIRE_HASH`, computed by
  CMake) travels in every `Announce` and the hub flags `wireMismatch`.
  `protocol/ota_image.hpp` keeps what is not wire (the on-flash image
  header). External processes (Godot plant, hub, ESP32 relay) speak ONLY
  the wire, inside transport frames, and never link flight-core. The hub
  is a GATEWAY: its websocket carries binary `GatewayMessage`s of the
  second schema, `gateway.proto` (a transport `Frame` = src, dst, one
  encoded Envelope, forwarded both ways without interpretation; the
  `NodeTable`; `NodeTelemetry`, one node's whole measure table as the
  gateway pulled it; the gateway-local services: `OtaCommand`/`OtaState`,
  `ProfileCommand`; `Ack`), never JSON. Every body of that oneof shares one
  nanopb struct, so anything per-node and unbounded gets a message of its
  own rather than a field in `Node`. The hub also has an HTTP side, which
  is filesystem-only by invariant:
  `/api/telemetry/{configs,exports}` reads and writes files under
  `HttpConfig::telemetryDir` (`logs/telemetry`), and touches no registry
  and no counter. The web pages
  (`software/hub/pages/`) generate their own TypeScript codec of both
  schemas at build time (`pnpm gen`, protoc-gen-es) and model the system
  as nodes by node id, never by kind or "connection". protocol/ is payload
  only: it names no address and no route. See
  `software/components/protocol/README.md` and `software/hub/README.md`.
- `transport/` - static lib, the interface manager between processes and
  boards (`software/components/transport/README.md`). Frames an opaque
  payload with `src u32, dst u32, seq u16, hops u8`, learns every node from
  any frame heard, beacons once per second, expires silent nodes, relays
  between links when asked. Depends on nothing but `drone_warnings`
  (`transport/serial_framing.hpp` lives here, the one CRC-16 of the
  project). The core and `UartLink` build for stm32 (no heap, fixed
  tables, function pointer callbacks); `UdpLink` needs BSD sockets
  (desktop, and lwIP on the ESP32): one shared discovery port (47820) for
  broadcasts, one ephemeral data socket per node for unicasts. Node ids
  are self-assigned `uint32_t` (random on desktop, `hashNodeId()` of the
  MCU UID on a board, of the MAC on the ESP32), never configured.
  Adopted by every node: drone_sim and the hub over UDP, the batch
  campaign, the Godot plant (a GDScript port,
  `sim-godot/scripts/transport/transport.gd`, kind `plant`: it hosts one
  virtual drone per `drone_sim` node it hears and the lockstep sensor /
  actuator exchange is unicast frames between the two node ids, no port
  of its own), and the firmware as a node with one `UartLink` on USART1
  (`Uart1Stream` over the uart1 rings; frames travel in the serial
  framing `A5 5A len_lo len_hi payload crc16`, 512 bytes at most). The
  ESP32 riding the drone (`esp32-bridge/`) is a transport relay and a
  node: two links (`UartLink` to the board, the shared `UdpLink` on lwIP),
  `setRelay(true)`, a beacon of its own (kind `RELAY`, mcu `ESP32C3`) on
  both links, and a `setRelayFilter()` towards the UART that lets unicasts
  and `Announce` broadcasts through and keeps every other LAN broadcast
  off the line; it logs through the log library, its lines and module
  table going out on the LAN link alone (the optional link mask of
  `send()`), and answers the `LogControl` addressed to it. It compiles
  `transport.cpp`, `uart_link.cpp`, `posix/udp_link.cpp`, the log library
  and the nanopb codec straight from `software/components/`. The hub holds
  one `UdpLink`, relays nothing, and sees the board and the relay as two
  more nodes (kinds `firmware` and `relay`, their own Announces, at the
  relay's address). The firmware broadcasts everything
  it emits (telemetry, answers, `Log` lines through the log library's
  `TransportSink`) and takes commands through `CommandReceiverTransport`
  fed by `Transport::poll()` once per flight frame.
- `telemetry/` - the registry of named float measures every node exposes
  (`software/components/telemetry/README.md`). A leaf like `log` (static
  lib, `drone_warnings` alone, no heap, no iostream, builds for stm32): it
  knows names, units and where values live, and nothing of the wire. A
  measure is a `TelemetryEntry` declared as a member next to the variable
  it reads (a pointer to a float, or a context plus a reader for an enum,
  a boolean or a u64); entries link in construction order, because a wire
  id is an index into that order, and unlink in their destructor. Names are
  hierarchical lowercase paths (`estimator/altitude`, `rate/roll/p_term`),
  at most `MAX_TELEMETRY_NAME` = 40 characters, and the name is the stable
  identity across reboots. `MAX_TELEMETRY_ENTRIES` = 128 is the size of the
  table the wire adapter (`platform_common/telemetry_service.hpp`) freezes
  in `init()`. Adding a measure is one line where the value is computed:
  nothing in the schema, the packer or any codec changes.
- `log/` - the logging library of every node
  (`software/components/log/README.md`). `log` is a leaf (static lib,
  `drone_warnings` alone, no heap, no iostream): `LogModule` declared once
  per source file as a static object with a `uint16_t` id and a
  hierarchical name (`platform/imu`, `ota/updater`, `app/boot`), linked
  into an intrusive registry; five levels `TRACE..ERROR` checked before
  `vsnprintf` into 96 bytes; up to 2 `AbsLogSink`s (`ConsoleSinkPosix` on
  desktop, `RttSink` in platform_stm32); the clock is a registered
  function pointer, the library never reads one. `log_wire` (links
  `protocol`) adds `TransportSink` (a `Log` envelope per line, 50 lines/s,
  drops reported as a `log/core` WARN), `logPublishModules()` (the node's
  table as `LogModules` pages) and `logHandleControl()` (`LogControl`:
  query or set one module). Shared files take their ids from
  `log/module_ids.hpp`, each app its own from its `log_modules.hpp` (from
  256). The gateway keeps every node's table (`Node.log_modules`), queries
  a node when it appears, and is a node itself: its lines go out as `Log`
  frames from its own id (mirrored to its clients, never fed back to
  itself). The pages toast WARN/ERROR only, module name resolved from
  the table.

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
changes; `ci.yml` runs 8 jobs (desktop+tests+batch through headless Godot, stm32,
esp32, desktop-san, pages pnpm typecheck+build+test, mobile flutter
analyze+apk, format+ascii, tidy desktop+stm32) inside that image, pinned by
digest.
After a `.devcontainer/` change, bump the digest in `ci.yml` to the one the
image workflow pushed (`docker manifest inspect ...:latest`). Container jobs need `options: --user root` (the
image defaults to user `dev`, the runner mounts workdirs for another UID).
If CI needs a new tool, add it to the Dockerfile; the image is the single
source of truth for the environment (same versions as the devcontainer).
