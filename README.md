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

## Modules

- `flight-core/` - pure flight core: no dynamic allocation, no
  exceptions/RTTI, no clock access. `float` everywhere
  (`-Wdouble-promotion` as an error).
- `platform/` - 6 abstract interfaces (AbsSensorSource, AbsMotorSink,
  AbsCommandReceiver, AbsTelemetrySender, AbsLogSink, AbsClock) plus one
  implementation set per variant. No singletons: each executable has an
  explicit composition root in its main.
- `protocol/` - header-only library, versioned packed structs, spoken by
  everyone (firmware, sim, Godot, ground station).

## Build

### Inside the devcontainer (recommended)

Open the folder in VS Code -> "Reopen in Container". Both toolchains
(native gcc + arm-none-eabi) and the pinned tooling live in the image.

```sh
# Desktop: build + tests
cmake --preset desktop && cmake --build --preset desktop && ctest --preset desktop

# Sanitizers (ASan/UBSan)
cmake --preset desktop-san && cmake --build --preset desktop-san && ctest --preset desktop-san

# STM32F405 cross-compilation (Cortex-M4F) -> firmware.elf
cmake --preset stm32 && cmake --build --preset stm32

# Sign of life
./build/desktop/apps/drone_sim/drone_sim        # 500 iterations by default
```

### Manual build (outside the container)

Prerequisites: CMake >= 3.25, Ninja, gcc, arm-none-eabi-gcc (ARM tarball
14.2.rel1), clang-format/clang-tidy 21 for the checks. Same commands as above.

### Code quality

```sh
git ls-files '*.cpp' '*.hpp' '*.c' '*.h' | xargs clang-format --dry-run --Werror
run-clang-tidy -p build/desktop "$(pwd)/(apps|flight-core|platform|protocol|tests)/"
```

## CI

- `devcontainer-image.yml` - rebuilds the image and pushes it to GHCR
  (`ghcr.io/keyrim/drone-mark4-devcontainer`) whenever `.devcontainer/` changes.
- `ci.yml` - 5 parallel jobs inside that image: desktop+tests, stm32,
  desktop-san, clang-format, clang-tidy.

First-push bootstrap: the `ci.yml` jobs pull the GHCR image - on the very
first push, wait for `devcontainer-image` to finish, then re-run `ci` if
needed (and make the GHCR package public to avoid pull issues).

## J-Link license note

The development image downloads the J-Link tools from segger.com: the
automated download (`--post-data accept_license_agreement=accepted`) implies
acceptance of the [Segger license](https://www.segger.com/downloads/jlink).
If that is a problem, comment out the J-Link layer in the `Dockerfile` or
install the .deb manually. `JLinkGDBServer` runs on the HOST (USB probe);
inside the container, `gdb-multiarch` connects to it with
`target extended-remote host:2331`.

## License

[MIT](LICENSE).
