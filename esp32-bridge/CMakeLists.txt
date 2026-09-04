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

# The build identity of this image, the same two values the flight images
# carry (scripts/make_ota.py): the configure time and the short commit hash.
# Both are read at configure, so a reconfigure is what refreshes them. They
# reach the code twice: as compile definitions for the Announce (main/), and
# as PROJECT_VER, which ESP-IDF stamps into the application description of
# the image, where the firmware store reads a slot's identity back from
# ("<buildEpoch>-<gitHash>", 19 characters of the 32 the field holds).
string(TIMESTAMP BRIDGE_BUILD_EPOCH "%s" UTC)
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=8 HEAD
                OUTPUT_VARIABLE BRIDGE_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT BRIDGE_GIT_HASH)
    set(BRIDGE_GIT_HASH "00000000")
endif()
set(PROJECT_VER "${BRIDGE_BUILD_EPOCH}-${BRIDGE_GIT_HASH}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_bridge)

# The .ota bundle the hub sends to the relay, packaged next to the image on
# every build (scripts/make_ota.py in its esp32 mode: the raw application
# image as it is, one for both slots, under a manifest naming the chip, the
# wire hash of components/mark4_proto and the identity stamped above).
idf_build_get_property(bridge_python PYTHON)
idf_build_get_property(bridge_build_dir BUILD_DIR)
idf_build_get_property(bridge_wire_hash DRONE_WIRE_HASH_HEX)
add_custom_command(TARGET gen_project_binary POST_BUILD
    COMMAND ${bridge_python} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/make_ota.py
            --esp32 ${bridge_build_dir}/${CMAKE_PROJECT_NAME}.bin
            --name ${CMAKE_PROJECT_NAME}
            --build-epoch ${BRIDGE_BUILD_EPOCH}
            --git-hash ${BRIDGE_GIT_HASH}
            --wire-hash ${bridge_wire_hash}
            --outdir ${bridge_build_dir}
    COMMENT "packaging ${CMAKE_PROJECT_NAME}.ota"
    VERBATIM)
