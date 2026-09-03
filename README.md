# drone-mark4 - "throw-drone"

[![ci](https://github.com/Keyrim/drone-mark4/actions/workflows/ci.yml/badge.svg)](https://github.com/Keyrim/drone-mark4/actions/workflows/ci.yml)
[![devcontainer-image](https://github.com/Keyrim/drone-mark4/actions/workflows/devcontainer-image.yml/badge.svg)](https://github.com/Keyrim/drone-mark4/actions/workflows/devcontainer-image.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A 5-inch racing drone is thrown by hand, motors off. The firmware detects the
throw, predicts the apex of the parabola, spins the motors up ahead of time,
recovers from an arbitrary attitude (the drone may tumble) and settles into a
stable hover. Everything is rewritten from scratch - firmware, simulator,
tooling - as a deliberate learning project. Behavioral reference: ArduPilot's
"Throw Mode".

Documentation:

- [docs/architecture.md](docs/architecture.md) - system architecture (diagrams)
- [docs/plan-dev.md](docs/plan-dev.md) - development plan and reference document
- [docs/contributing/cpp-guidelines.md](docs/contributing/cpp-guidelines.md) - C++ coding guidelines
- [docs/mobile-app.md](docs/mobile-app.md) - phone as gateway: what the two mobile PoCs established, and the roadmap of `software/mobile`

## Modules

- `flight-core/` - pure flight core: no dynamic allocation, no
  exceptions/RTTI, no clock access. `float` everywhere
  (`-Wdouble-promotion` as an error).
- `platform/` - 5 abstract interfaces (AbsSensorSource, AbsMotorSink,
  AbsCommandReceiver, AbsLogSink, AbsClock) plus one implementation set per
  variant. No singletons: each executable has an
  explicit composition root in its main.
- `protocol/` - one protobuf schema (`mark4.proto`), codecs generated at
  build time for C/C++ (nanopb), GDScript (godobuf) and python; spoken by
  everyone (firmware, sim, Godot, hub).
- `telemetry/` - the registry of named measures a node exposes, declared
  next to the variables they read. A leaf: names, units and pointers, no
  wire. A ground tool discovers the list and streams the subset it wants.
- `transport/` - the interface manager: frames every payload with a source
  and a destination node, learns every node it hears, relays between links.
- `log/` - one logging module per source file, filterable at runtime, its
  lines going out on the wire like everything else.

## Build

### Inside the devcontainer (recommended)

Open `drone-mark4.code-workspace` in VS Code -> "Reopen in Container". The
workspace exposes the repo root and `software/` (the CMake project root) as
folders and carries the debug configurations. Both toolchains (native gcc +
arm-none-eabi) and the pinned tooling live in the image.

```sh
# Desktop: build + tests
cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop

# Sanitizers (ASan/UBSan)
cmake --preset desktop-san && cmake --build --preset desktop-san && ctest --preset desktop-san

# STM32F405 cross-compilation (Cortex-M4F) -> firmware.elf
cmake --preset stm32 && cmake --build --preset stm32

# Sign of life (waits for UDP sensor packets, exits after 2 s of silence)
./software/build/desktop/drone_sim/drone_sim        # 500 frames max by default
```

### Mobile app (Flutter, Android)

`software/mobile/` is the Flutter project; the image carries Flutter, the
Android SDK, the NDK and adb. The phone is reached over Wi-Fi (developer
options -> wireless debugging), the container being on the host network:

```sh
./scripts/adb_wifi.sh                     # discovers the phone over mDNS, pairs once, connects
cd software/mobile && flutter pub get && ./tool/gen.sh   # generated codec, wire hash, ffi binding
flutter run                               # or: flutter build apk --debug --target-platform android-arm64
```

### Manual build (outside the container)

Prerequisites: CMake >= 3.25, Ninja, gcc, arm-none-eabi-gcc (ARM tarball
14.2.rel1), clang-format/clang-tidy 21 for the checks. Same commands as above.

### Code quality

```sh
git ls-files '*.cpp' '*.hpp' '*.c' '*.h' | xargs clang-format --dry-run --Werror
run-clang-tidy -p software/build/desktop "$(pwd)/(apps|flight-core|platform|protocol|tests)/"
```

## Simulation chain

Three processes are transport nodes on udp/47820 and find each other by
their beacons: the Godot plant spawns one virtual drone per `drone_sim`
it hears and feeds it sensor frames in lockstep, `drone_sim` answers with
actuator frames and broadcasts telemetry to any node, the hub decodes it
for the pages. Nothing is configured, start them in any order.

```sh
# Terminal 1 - flight process (one per virtual drone wanted)
./software/build/desktop/drone_sim/drone_sim

# Terminal 2 - the plant (the desktop build generated its codec)
godot --path sim-godot

# Terminal 3 - decoding endpoint and web pages on http://127.0.0.1:47810
./software/build/desktop/hub/hub
```

Every python tool in the repository runs on the standard library alone, so
there is nothing to install first.

## Debugging from VS Code

`drone-mark4.code-workspace` provides:

- `drone_sim (gdb)` - debugs the CMake launch target: pick the `desktop`
  preset and the `drone_sim` target in the CMake Tools status bar, the
  target is rebuilt automatically before each launch;
- `sim_stub (python)` - streams frames until stopped.

The default build task (Ctrl+Shift+B) builds the active CMake configure
preset.

## CI

- `devcontainer-image.yml` - rebuilds the image and pushes it to GHCR
  (`ghcr.io/keyrim/drone-mark4-devcontainer`) whenever `.devcontainer/` changes.
- `ci.yml` - 8 parallel jobs inside that image: desktop+tests+batch, stm32,
  esp32, desktop-san, pages, mobile (gen, flutter analyze, format, test, debug apk),
  clang-format+ascii, clang-tidy.

First-push bootstrap: the `ci.yml` jobs pull the GHCR image - on the very
first push, wait for `devcontainer-image` to finish, then re-run `ci` if
needed (and make the GHCR package public to avoid pull issues).

## J-Link license note

The development image downloads the J-Link tools from segger.com: the
automated download (`--post-data accept_license_agreement=accepted`) implies
acceptance of the [Segger license](https://www.segger.com/downloads/jlink).
If that is a problem, comment out the J-Link layer in the `Dockerfile` or
install the .deb manually. The whole J-Link stack (`JLinkExe`,
`JLinkGDBServer`, `JLinkRTTClient`) runs inside the container: the probe is
attached to WSL with `usbipd` and reaches the container through the `/dev`
bind mount (see `docs/bring-up.md`).

## License

[MIT](LICENSE).
