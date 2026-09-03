# The two INTERFACE targets every component links, defined once and included
# by every CMake project that builds components of software/: the root
# project here, and the mobile app's native library, which Gradle configures
# as a project of its own with the Android NDK toolchain.

# Warnings shared by all project code (not by FetchContent dependencies).
add_library(drone_warnings INTERFACE)
target_compile_options(drone_warnings INTERFACE -Wall -Wextra -Wdouble-promotion -Werror)

# flight-core and platform: no exceptions, no RTTI, on every preset.
add_library(drone_strict INTERFACE)
target_compile_options(drone_strict INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
