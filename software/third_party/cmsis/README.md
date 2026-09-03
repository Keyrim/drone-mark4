# Vendored CMSIS headers

Verbatim copies of the CMSIS headers the stm32 build needs: the STM32F405
device header (register map, IRQ numbers) and the Cortex-M4 core support it
includes. Vendored on purpose instead of FetchContent: the upstream repos
are large and would re-download after every clean build, while the firmware
needs exactly these files.

Do not edit anything under this directory. The repo hard rules (ASCII-only,
clang-format) do not apply to vendored files: `scripts/check_ascii.sh`
excludes `software/third_party` and `software/third_party/.clang-format`
sets `DisableFormat`.

The `cmsis_f4` INTERFACE target
(`software/components/platform/src/stm32/CMakeLists.txt`) adds both
directories as SYSTEM include paths and defines `STM32F405xx`; it exists
only in the stm32 preset.

## device/

From https://github.com/STMicroelectronics/cmsis_device_f4,
tag `v2.6.11`, commit `0fa0e489e053fa1ca7790bb40b4d76458f64c55d`.
License: Apache-2.0 (see `device/LICENSE.md`, copied from the same tag).

| File                  | Upstream path                 |
| --------------------- | ----------------------------- |
| `stm32f405xx.h`       | `Include/stm32f405xx.h`       |
| `system_stm32f4xx.h`  | `Include/system_stm32f4xx.h`  |
| `LICENSE.md`          | `LICENSE.md`                  |

## core/

From https://github.com/ARM-software/CMSIS_5,
tag `5.9.0`, commit `2b7495b8535bdcb306dac29b9ded4cfb679d7e5c`.
License: Apache-2.0 (see `core/LICENSE.txt`, copied from the same tag).

| File               | Upstream path                        |
| ------------------ | ------------------------------------ |
| `core_cm4.h`       | `CMSIS/Core/Include/core_cm4.h`      |
| `cmsis_version.h`  | `CMSIS/Core/Include/cmsis_version.h` |
| `cmsis_compiler.h` | `CMSIS/Core/Include/cmsis_compiler.h`|
| `cmsis_gcc.h`      | `CMSIS/Core/Include/cmsis_gcc.h`     |
| `mpu_armv7.h`      | `CMSIS/Core/Include/mpu_armv7.h`     |
| `LICENSE.txt`      | `LICENSE.txt`                        |

`mpu_armv7.h` is required because `stm32f405xx.h` sets `__MPU_PRESENT` to 1
and `core_cm4.h` then includes it. This is the complete closure: a
translation unit defining `STM32F405xx` and including `<stm32f405xx.h>`
compiles with these two directories alone.
