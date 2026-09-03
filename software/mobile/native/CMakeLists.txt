# Native library of the mobile app: the project's transport compiled for
# Android by the NDK, behind a C ABI Dart reaches through dart:ffi. Gradle
# configures this file as a CMake project of its own (externalNativeBuild in
# android/app/build.gradle.kts), once per ABI.
#
# The transport is not copied and its sources are not listed here: the
# component's own CMakeLists.txt is added from software/components, with the
# same warning targets as the desktop and stm32 builds, so a file added to
# the component is built into the app without touching anything on this side.
cmake_minimum_required(VERSION 3.22)

project(mark4_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The static component is linked into a shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DRONE_SOFTWARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Bionic has the BSD sockets the UDP link needs: the component selects its
# POSIX sources on this value.
set(DRONE_PLATFORM "android")

include(${DRONE_SOFTWARE_DIR}/cmake/drone_targets.cmake)
add_subdirectory(${DRONE_SOFTWARE_DIR}/components/transport transport)

add_library(mark4_transport SHARED transport_shim.cpp)
target_include_directories(mark4_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mark4_transport PRIVATE transport drone_warnings drone_strict)
