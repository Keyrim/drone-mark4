# arm-none-eabi toolchain - generic Cortex-M4F build (STM32F405).
# Proves that flight-core cross-compiles; board bring-up comes with the real
# drivers.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)

# No OS: CMake's try_compile must produce a lib, not an executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(DRONE_MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# -g costs nothing on target: debug info lives in the ELF only, never in
# the sections loaded to flash. It feeds gdb/addr2line on every build type.
set(CMAKE_C_FLAGS_INIT "${DRONE_MCU_FLAGS} -g -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT
    "${DRONE_MCU_FLAGS} -g -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")

# newlib-nano + nosys stubs; startup provided by platform_stm32 (-nostartfiles).
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${DRONE_MCU_FLAGS} --specs=nano.specs --specs=nosys.specs -nostartfiles -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
