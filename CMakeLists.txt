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

add_subdirectory(protocol)
add_subdirectory(flight-core)
add_subdirectory(platform)
add_subdirectory(apps)

if(DRONE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/golden)
endif()
