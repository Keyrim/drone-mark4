cmake_minimum_required(VERSION 3.16)

# Standard ESP-IDF project: the IDF build system owns the toolchain, the
# component discovery and the flash image layout. Two components: main (the
# WiFi bring-up and the relay, with the shared transport sources compiled
# in) and components/mark4_proto (nanopb runtime + the codec generated from
# software/components/protocol/mark4.proto at build).
# Credentials of the network the relay joins before falling back to its own
# access point. They belong to whoever builds, not to the repository, so they
# live in an untracked local.cmake next to this file:
#     set(BRIDGE_STA_SSID "my-network")
#     set(BRIDGE_STA_PASSWORD "my-password")
# Without the file, the relay is an access point and nothing else.
include(local.cmake OPTIONAL)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_bridge)
