# "throw-drone" project - Reference document

> Project reference document and initialization brief. Section 10 describes
> the exact scope of milestone 0; everything else is the context that
> justifies and constrains the choices. An agent tasked with initializing the
> project must read everything, then implement ONLY milestone 0.

> **Project name: `drone-mark4`.** "throw-drone" is the concept (the target
> flight mode); every named artifact - repo, devcontainer image, Docker
> volumes, GHCR image - uses `drone-mark4`.

---

## 1. Project vision

A 5-inch racing drone is thrown by hand, motors off. The firmware detects the
throw, predicts the apex of the parabola, spins the motors up ahead of time,
recovers from an arbitrary attitude (the drone may tumble) and stabilizes
into a hover.

Structuring physical constraints:

- A hand throw = 5 to 8 m/s vertical, apex reached in 0.5 to 0.8 s. The whole
  detection -> decision -> spin-up chain must fit within that budget.
- In free fall the accelerometer reads ~0 g (specific force): attitude
  estimation relies on pure gyro integration during the ballistic phase.
- The release velocity is estimated by integrating the accelerometer DURING
  the throw's thrust phase (2-5 g); then vz(t) = vz0 - g*t and
  t_apex = vz0/g. Spin-up is triggered at t_apex - motor_latency
  (~100-300 ms); the apex is never "detected" after the fact.
- Flight state machine: idle -> armed motors off -> ballistic -> spin-up ->
  attitude recovery (quaternion) -> hover. Plus a safety cutoff (timeout, gyro
  saturation, impact).
- Safety: 5" propellers are dangerous. A complete throw signature is required
  (sustained >2 g peak followed by confirmed <0.3 g free fall), an
  incompressible delay before the first propeller turn, an RC kill switch
  handled at the very top of step(), never over the WiFi link.

Existing reference: ArduPilot's "Throw Mode" (mode_throw.cpp) does exactly
this and serves as the behavioral reference. The goal here is to rewrite
EVERYTHING from scratch (firmware, sim, tools) - this is a deliberate
learning project.

## 2. Target hardware

- Off-the-shelf reflashable FC (DFU bootloader, SWD pads), no custom PCB.
  Candidates: SpeedyBee F405 V4 (stack ~80 EUR), Matek H743-Mini V3. Starting
  on **STM32F405**; the code must not assume the exact model (the stack
  choice is a detail of `software/components/platform/src/stm32/`).
- Modern SPI IMU (ICM-42688-P: +/-4000 deg/s - the MPU6050 is excluded, its
  +/-2000 deg/s range is insufficient for a spinning throw), barometer
  (DPS310/SPL06), SPI flash or microSD for the blackbox, 4-in-1 DShot ESC.
- Telemetry bridge: an ESP32 on a free FC UART (921600 baud to 2 Mbaud),
  transparent UART <-> UDP WiFi bridge. BLE is excluded (insufficient
  throughput).
- Debug: **J-Link** probe (GDB server + RTT). Known F405 trap: the CCM RAM
  (64 KB) is not DMA-accessible - DMA buffers (DShot, SPI) outside CCM in the
  linker script.

## 3. Software architecture - principles

### 3.1 flight-core

- **Pure** static C++ library: no system dependency, no dynamic allocation in
  the loop, no exceptions or RTTI, no clock access, no waiting.
- Central API: `FlightCore::step(const SensorFrame&, ActuatorFrame&)` - a
  synchronous, single-threaded function paced by data arrival (not by time).
  The timestamp travels INSIDE the SensorFrame, stamped by platform at
  acquisition.
- No RTOS. On STM32: minimalist ISRs (move bytes into ring buffers), main
  loop in WFI woken by the gyro data-ready. On Linux: blocking recv(). Same
  loop structure on both sides.
- Quaternions everywhere (arbitrary attitude, no Euler). Estimator: Mahony +
  pure gyro integration during the ballistic phase; vertical fusion of baro +
  accelerometer.
- The kill switch / RC state is a SensorFrame field, handled first in step().
- Numerical discipline: `float` everywhere, `f` suffix on every constant,
  `-Wdouble-promotion` treated as an error on all presets. Goal: comparable
  execution between desktop (double FPU) and F405 (single-precision FPU).

### 3.2 platform - services behind virtual interfaces

- Public interfaces live in `software/components/platform/include/platform/*.hpp`: abstract
  classes, one per service. Implementations live in `software/components/platform/src/<variant>/`
  and inherit from them (ArduPilot's AP_HAL model).
- Include pattern: `target_include_directories` on `include/` ->
  `#include "platform/xxx.hpp"`.
- Base classes stay **pure** (no template method); shared code goes into
  composed helpers. A variant's private services (e.g. the sim's com service)
  keep their headers under `software/components/platform/src/<variant>/include/`, never exposed.
- No singletons: an explicit composition root per application (an App/Board
  object built in the main, services as members, initialization in a written
  order, injection by reference into FlightCore). Static instantiation on
  STM32.
- Services are composable across variants: the "sim on STM32 target" build
  mixes ClockStm32 + UART transport + SensorSourceSim.

Base services (a short list, deliberately):

| Service           | Direction              | stm32                    | sim                    | replay              |
|-------------------|------------------------|--------------------------|------------------------|---------------------|
| `SensorSource`    | input, **blocking**    | IMU/baro/RC, ISR + WFI   | sim link packets       | frames from file    |
| `MotorSink`       | output, non-blocking   | DShot DMA                | response to the sim    | ignored (open loop) |
| `CommandReceiver` | input, polled per tick | UART from ESP32          | UDP/TCP                | absent              |
| `TelemetrySender` | output                 | UART to ESP32            | UDP broadcast          | UDP broadcast       |
| `LogSink`         | output                 | SD / flash               | file                   | null                |
| `Clock`           | internal to platform   | hardware timers          | sim clock / lockstep   | logged timestamps   |

`SensorSource::waitFrame()` is the system's single wait point. `Clock` is
never passed to FlightCore (it serves the platform services between
themselves).

### 3.3 protocol

- Header-only INTERFACE lib, zero dependency, included by everyone
  (flight-core, firmware, desktop, ESP32) and parsed by Godot and the ground
  station.
- Packed structs, frozen units and sizes, a version number in the first byte
  of every packet, checked by all consumers.
- Distinct from `flight_core/types.hpp` (internal types free to evolve).
  flight-core converts internal -> wire in telemetry.cpp.
- Three families: telemetry (state, attitude, motors - UDP broadcast,
  multi-consumer), commands (PID params, triggers, scenario reset), sim link
  (SensorFrame/ActuatorFrame on the wire + real-time or **lockstep** mode:
  the sim waits for the motor response before advancing its physics ->
  determinism, accelerated runs, debugger single-stepping).

### 3.4 Executables and processes

Each executable = flight-core + one composition of platform services (tiny
main):

- `firmware` - stm32 preset only.
- `drone_sim` - desktop AND stm32 presets (numerical conformity test of
  sim-on-target).
- `drone_replay` - desktop preset. Replays a blackbox in open loop,
  re-publishes telemetry over UDP during the replay (option
  `--speed 0.1 / 1.0 / max`).
- `drone_batch` - NOT a separate executable anymore. Monte Carlo campaigns
  run through the real simulator: `tools/batch/run_batch.py` spawns N pairs
  of (headless Godot, drone_sim) in lockstep, faster than real time, resets
  and throws through the sim command channel, and judges outcomes from the
  telemetry. Rationale: a second internal C++ physics would fork the truth -
  the validation reference must be the ONE physics that has the collisions
  and the calibrated sensor models. A dedicated C++ physics only becomes
  worth it for massive parameter sweeps, if that need ever materializes.

External processes (do NOT link flight-core, speak only protocol/):

- **Godot simulator** (`sim-godot/`, a standalone project, runs on the HOST,
  not in the container): RigidBody3D + Jolt physics, 500-1000 Hz physics
  tick, default damping at 0, manually defined inertia, first-order motor
  model (tau ~ 20-40 ms), k*omega^2 thrust, realistic sensor models - **the
  accelerometer simulates specific force** (0 g in free fall),
  noise/bias/quantization/real rates. Parameterizable "throw" command.
  "Viewer" mode: physics off, pose slaved to received telemetry (replay of
  real flights).
- **Hub + web pages** (`software/hub/`, `software/hub/pages/`): session launcher,
  discovery, recording; real-time plots, 3D attitude view and command
  console served as web pages over one HTTP/WebSocket port.
- **ESP32 bridge** (`esp32-bridge/`).

Telemetry is emitted as **UDP broadcast/multicast**: any combination of tools
listens simultaneously, whether the source is the firmware (via ESP32),
drone_sim or drone_replay.

## 4. Target tree

```
drone-mark4/
|-- software/
|   |-- CMakeLists.txt
|   |-- CMakePresets.json          # desktop, desktop-san, stm32
|   |-- apps.json
|   |-- cmake/toolchain-arm-none-eabi.cmake
|   |-- components/
|   |   |-- protocol/              # INTERFACE lib
|   |   |   `-- include/protocol/{version,telemetry,commands,sim_link}.hpp
|   |   |-- flight-core/
|   |   |   |-- include/flight_core/{flight_core,types}.hpp
|   |   |   `-- src/               # flight_core.cpp, state_machine.cpp,
|   |   |                          # estimator/{attitude,vertical}.cpp,
|   |   |                          # control/{rate_pid,attitude_ctrl,mixer}.cpp,
|   |   |                          # throw_launch/{detector,apogee}.cpp,
|   |   |                          # telemetry.cpp, blackbox.cpp
|   |   `-- platform/
|   |       |-- include/platform/  # abstract interfaces (the 6 services)
|   |       `-- src/
|   |           |-- common/        # composed helpers, if any
|   |           |-- stm32/         # stm32 preset only (+ private include/)
|   |           |-- sim/           # both presets (+ private include/, udp/uart transports)
|   |           `-- replay/        # desktop preset
|   |-- {drone_firmware,drone_sim,drone_replay,hub}/
|   `-- tests/{unit,golden,scenarios}/
|-- tools/
|-- sim-godot/
|-- esp32-bridge/
`-- logs/{blackbox,streams,batch}/ # gitignored
```

CMake logic: `platform` publishes an INTERFACE target (headers) that
flight-core links; the implementation libs (`platform_stm32`, `platform_sim`,
`platform_replay`) are only declared in the presets where they make sense,
and are linked by the apps.

## 5. Development environment

- Host: native Linux or **WSL2** (no native Windows support). VSCode +
  devcontainer.
- **A single devcontainer** with both toolchains. Godot stays on the host
  (heavy graphical editor, godot-tools LSP connected over TCP, port 6005 to
  forward; two VSCode windows: one attached to the container for C++, one on
  the host for sim-godot/).
- X11/WSLg for windowed Godot from the container (`--device=/dev/dri`
  for the GPU).
- Network: `--network=host` (simplest for container <-> host UDP broadcast).
- Flash/debug: `JLinkGDBServer` launched on the HOST, `gdb-multiarch` in the
  container with `target extended-remote host:2331`. RTT for bring-up traces.

### Dockerfile - precise requirements

- **Ubuntu 24.04** base.
- Clean, readable, in commented logical layers; direct installations (no
  obscure external scripts).
- git + base tools (curl, wget, unzip, ca-certificates, pkg-config...).
- Distro-native **gcc** (desktop build - gcc everywhere, no clang for
  compiling).
- **arm-none-eabi-gcc: official ARM tarball** (not the Ubuntu package),
  pinned version, installed under /opt with PATH configured.
- **Recent CMake + Ninja.**
- **clang-format and clang-tidy from the LLVM apt repository (apt.llvm.org),
  pinned version** - not the distro packages. Alternatives via
  update-alternatives or symlinks exposing `clang-format`/`clang-tidy`
  without a suffix.
- gdb-multiarch; **J-Link tools** (Segger .deb package - note in the README
  that the user must accept the Segger license; provide the build argument or
  documented step if the automated download is a problem).
- Python 3 (stdlib-only scripts); node + pnpm (web pages).
- **Non-root user with NOPASSWD sudo**, UID/GID mapped for the devcontainer.

## 6. Build

- CMake >= 3.25 + **Ninja**, presets:
  - `desktop`: gcc, RelWithDebInfo, tests enabled.
  - `desktop-san`: gcc + `-fsanitize=address,undefined` (ASan/UBSan), tests.
  - `stm32`: arm-none-eabi toolchainFile, `-fno-exceptions -fno-rtti`,
    newlib-nano, no tests.
- Common flags: `-Wall -Wextra -Wdouble-promotion -Werror`.
- `CMakePresets.json` versioned; `CMakeUserPresets.json` gitignored.
- C++20.

## 7. CI/CD (GitHub Actions - public project)

- Workflow 1: build the devcontainer image -> push to **GHCR** whenever the
  Dockerfile changes. All other jobs run INSIDE that image (a single source
  of truth for the environment).
- Workflow 2 (PR + main), parallel jobs:
  1. build `desktop` preset + unit tests;
  2. build `stm32` preset (guarantees flight-core stays compilable for the
     embedded target);
  3. build `desktop-san` preset + tests under sanitizers;
  4. `clang-format --dry-run --Werror` on all C++;
  5. `clang-tidy` over the desktop build's compile_commands.json.
- CI badge in the README. Free license (MIT or Apache-2.0) from the first
  commit.
- (Future, outside milestone 0: an optional Monte Carlo job (tools/batch/,
  headless Godot in the image) with a recovery
  rate threshold.)

## 8. Conventions

- Includes prefixed with the module name:
  `#include "flight_core/types.hpp"`,
  `#include "platform/sensor_source.hpp"`,
  `#include "protocol/telemetry.hpp"`.
- Headers in `.hpp`, sources in `.cpp`.
- `.clang-format` and `.clang-tidy` at the root; C++ conventions in
  `docs/contributing/cpp-guidelines.md` (`Abs` prefix for abstract classes,
  `m_camelCase` members, `UPPER_SNAKE` constants, Doxygen `@` notation).
- A single project namespace: `mark4` (no per-module namespaces).
- No `new`/`delete` in flight-core and platform; no iostream in flight-core.
- **English everywhere: code, identifiers, comments and documentation.**
- **ASCII only across the repository** (no em dashes, arrows, typographic
  quotes...); accented letters are tolerated in proper names.

## 9. Milestones (roadmap - context, do not implement beyond milestone 0)

- **M0 - Skeleton**: this document, section 10.
- **M1 - Tracer bullet**: a Python script simulates sinusoidal SensorFrames
  over UDP -> drone_sim -> broadcast telemetry -> embryonic ground station
  plotting one curve.
- **M2 - Godot**: real physics, sensor models (specific force!), throw
  command, file LogSink + blackbox format.
- **M3 - Estimator**: Mahony, gyro bias, vertical fusion, release velocity,
  apex prediction; drone_replay built in parallel; validation against Godot
  ground truth.
- **M4 - Flight**: hover first (rate PID, quaternion controller, mixer,
  altitude hold), then the full throw state machine; Monte Carlo campaigns
  through headless Godot (tools/batch/), run locally - a CI job with a
  recovery threshold is optional and deferred.
- **M5 - Real world** (in parallel from M2): board bring-up (SPI IMU, DShot,
  RTT, blackbox), detection validation with propellers removed, real hover,
  first throws.

## 10. MILESTONE 0 - exact scope for the initialization agent

Deliver a repo that compiles on all three presets with a green CI. Nothing
more.

### To create

1. The tree from section 4, with **almost-empty but real** libs:
   - `protocol/`: `version.hpp` (version constant), `sim_link.hpp` and
     `telemetry.hpp` with ONE minimal packed struct each (a few plausible
     fields, not the final format).
   - `flight-core/`: `types.hpp` (minimal SensorFrame/ActuatorFrame:
     timestamp, gyro[3], accel[3], baro, rc; motor outputs [4]),
     `platform.hpp` does NOT exist (interfaces belong to platform),
     `flight_core.hpp/.cpp` with a `FlightCore` class whose `step()` does
     something trivial but observable (counter, copy).
   - `software/components/platform/include/platform/`: the 6 abstract interfaces of section 3.2
     (pure virtual methods, virtual destructors, no logic).
   - `software/components/platform/src/sim/`: stub implementation of
     SensorSource/MotorSink/TelemetrySender/Clock running a loop without
     network for now (frames generated internally at a fixed rate) - just
     enough to prove the composition.
   - `software/drone_sim/`: a main with an explicit composition root (services
     built, injected into FlightCore, waitFrame -> step -> push loop) that
     runs, prints a sign of life and exits cleanly (iteration count as an
     argument, reasonable default).
   - `software/drone_firmware/`: a minimal stm32 main that compiles and links (empty
     loop + a platform_stm32 stub reduced to the strict minimum needed to
     link; NO real drivers).
   - `software/tests/unit/`: one real test (framework: Catch2 or GoogleTest via
     FetchContent) testing something real even if trivial (e.g.
     FlightCore::step increments its counter).
2. Root `CMakeLists.txt` + one per module, `CMakePresets.json` (desktop,
   desktop-san, stm32), `software/cmake/toolchain-arm-none-eabi.cmake`. The per-preset
   visibility logic of section 4. For the stm32 preset at milestone 0:
   generic Cortex-M4F compilation
   (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`), a minimal F405
   linker script and `--specs=nosys.specs` are enough - the goal is to prove
   flight-core cross-compiles, not to boot a board.
3. `.devcontainer/devcontainer.json` + `Dockerfile` per section 5 (precise
   requirements), port 6005 forwarded, `--network=host`, useful VSCode
   extensions (cmake-tools, clangd or cpptools, godot-tools).
4. `.clang-format`, `.clang-tidy`, `.gitignore` (build/, logs/,
   CMakeUserPresets.json...).
5. The GitHub Actions workflows of section 7 (GHCR image + the 5 jobs).
6. `README.md`: project description (recap the section 1 vision in a few
   lines), badges, build instructions (devcontainer and manual), a quick
   architecture sketch, the J-Link license note.
7. `LICENSE` (MIT).

### Acceptance criteria

- `cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop`: OK.
- Same for `desktop-san` (tests pass under sanitizers).
- `cmake --preset stm32 && cmake --build --preset stm32`: produces a
  firmware.elf.
- `./software/build/desktop/drone_sim/drone_sim` runs, prints its sign of life,
  exit code 0.
- clang-format and clang-tidy pass on all delivered code.
- The CI workflows are syntactically valid and reproduce exactly these
  commands.
- No singleton, no exceptions/RTTI in flight-core and platform, no `new`
  outside the desktop composition root, float constants suffixed with `f`.

### Out of scope for milestone 0 (DO NOT DO)

Real network/UDP, Godot, ground station, ESP32, real STM32 drivers,
estimator, control, flight state machine, blackbox format, final protocol.
Any head start on these topics will be redone - do not write it.
