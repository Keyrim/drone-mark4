# The relay: the WiFi bring-up (C, bridge_main.c), the relay itself
# (C++, relay.cpp), its firmware store over the ESP-IDF OTA partitions
# (firmware_store_esp32.cpp) and the shared sources it is built on, compiled
# here as they are (no copy): the transport core, the UART link and the
# BSD-socket UDP link, which lwIP serves like POSIX does, the log library,
# whose console sink is plain stdio and serves the USB serial port as it
# serves a desktop terminal, and the header-only update brick (ota/). The
# desktop-only node_id.cpp (/dev/urandom) stays out.
set(DRONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../software/components")
set(DRONE_TRANSPORT "${DRONE_ROOT}/transport")
set(DRONE_LOG "${DRONE_ROOT}/log")
set(DRONE_OTA "${DRONE_ROOT}/ota")

idf_component_register(SRCS "bridge_main.c"
                            "relay.cpp"
                            "firmware_store_esp32.cpp"
                            "${DRONE_TRANSPORT}/src/transport.cpp"
                            "${DRONE_TRANSPORT}/src/uart_link.cpp"
                            "${DRONE_TRANSPORT}/src/posix/udp_link.cpp"
                            "${DRONE_LOG}/src/module.cpp"
                            "${DRONE_LOG}/src/wire.cpp"
                            "${DRONE_LOG}/src/posix/console_sink_posix.cpp"
                       INCLUDE_DIRS "." "${DRONE_TRANSPORT}/include"
                                    "${DRONE_LOG}/include"
                                    "${DRONE_OTA}/include")

# The same warning set as the desktop and STM32 builds (drone_warnings), and
# the same freestanding C++ as the shared libraries (drone_strict).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wconversion -Wdouble-promotion -Werror
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# The build identity this node announces: the two values the top-level
# CMakeLists.txt computed at configure and stamped into the application
# description as PROJECT_VER, so the Announce and the slot identity the
# firmware store reads back can never disagree.
target_compile_definitions(${COMPONENT_LIB}
                           PRIVATE BRIDGE_BUILD_EPOCH=${BRIDGE_BUILD_EPOCH}U
                                   BRIDGE_GIT_HASH="${BRIDGE_GIT_HASH}")

# Set in the untracked local.cmake, see the top level CMakeLists.txt.
if(BRIDGE_STA_SSID)
    target_compile_definitions(${COMPONENT_LIB}
                               PRIVATE BRIDGE_STA_SSID="${BRIDGE_STA_SSID}"
                                       BRIDGE_STA_PASSWORD="${BRIDGE_STA_PASSWORD}")
endif()
