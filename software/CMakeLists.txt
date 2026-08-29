cmake_minimum_required(VERSION 3.25)

project(drone-mark4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DRONE_BUILD_TESTS "Build unit tests (desktop presets only)" OFF)

# The platform variant is named explicitly (set per preset): a second cross
# toolchain must never silently select the stm32 sources just because
# CMAKE_CROSSCOMPILING is true. The fallback inference only serves manual,
# preset-less configures.
set(DRONE_PLATFORM "" CACHE STRING "Platform variant to build: desktop or stm32")
if(DRONE_PLATFORM STREQUAL "")
    if(CMAKE_CROSSCOMPILING)
        set(DRONE_PLATFORM "stm32")
    else()
        set(DRONE_PLATFORM "desktop")
    endif()
    message(STATUS "DRONE_PLATFORM not set, inferred from the toolchain: ${DRONE_PLATFORM}")
endif()

# Warnings shared by all project code (not by FetchContent dependencies).
add_library(drone_warnings INTERFACE)
target_compile_options(drone_warnings INTERFACE -Wall -Wextra -Wdouble-promotion -Werror)

# flight-core and platform: no exceptions, no RTTI, on every preset.
add_library(drone_strict INTERFACE)
target_compile_options(drone_strict INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

add_subdirectory(components/protocol)
add_subdirectory(components/flight-core)
add_subdirectory(components/platform)

if(DRONE_PLATFORM STREQUAL "stm32")
    # Tools the flash images need, looked up once for every stm32 executable.
    set(DRONE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
    find_program(DRONE_ARM_SIZE arm-none-eabi-size)
    find_program(DRONE_ARM_OBJCOPY arm-none-eabi-objcopy)
    find_program(DRONE_PYTHON3 python3)
    add_subdirectory(drone_boot)
    add_subdirectory(drone_firmware)
elseif(DRONE_PLATFORM STREQUAL "desktop")
    add_subdirectory(drone_sim)
    add_subdirectory(hub)
else()
    message(FATAL_ERROR "unknown DRONE_PLATFORM '${DRONE_PLATFORM}'")
endif()

if(DRONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/golden)
endif()
