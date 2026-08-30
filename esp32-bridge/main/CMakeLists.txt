# The relay: the WiFi bring-up (C, bridge_main.c), the relay itself
# (C++, relay.cpp) and the shared sources it is built on, compiled here as
# they are (no copy): the transport core, the UART link and the BSD-socket
# UDP link, which lwIP serves like POSIX does, and the log library, whose
# console sink is plain stdio and serves the USB serial port as it serves a
# desktop terminal. The desktop-only node_id.cpp (/dev/urandom) stays out.
set(DRONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../software/components")
set(DRONE_TRANSPORT "${DRONE_ROOT}/transport")
set(DRONE_LOG "${DRONE_ROOT}/log")

idf_component_register(SRCS "bridge_main.c"
                            "relay.cpp"
                            "${DRONE_TRANSPORT}/src/transport.cpp"
                            "${DRONE_TRANSPORT}/src/uart_link.cpp"
                            "${DRONE_TRANSPORT}/src/posix/udp_link.cpp"
                            "${DRONE_LOG}/src/module.cpp"
                            "${DRONE_LOG}/src/wire.cpp"
                            "${DRONE_LOG}/src/posix/console_sink_posix.cpp"
                       INCLUDE_DIRS "." "${DRONE_TRANSPORT}/include"
                                    "${DRONE_LOG}/include")

# The same warning set as the desktop and STM32 builds (drone_warnings), and
# the same freestanding C++ as the shared libraries (drone_strict).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wconversion -Wdouble-promotion -Werror
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# The build identity this node announces, the same two values the flight
# images carry (scripts/make_ota.py): the configure time and the short commit
# hash. Both are read at configure, so a reconfigure is what refreshes them.
string(TIMESTAMP BRIDGE_BUILD_EPOCH "%s" UTC)
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=8 HEAD
                OUTPUT_VARIABLE BRIDGE_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT BRIDGE_GIT_HASH)
    set(BRIDGE_GIT_HASH "00000000")
endif()
target_compile_definitions(${COMPONENT_LIB}
                           PRIVATE BRIDGE_BUILD_EPOCH=${BRIDGE_BUILD_EPOCH}U
                                   BRIDGE_GIT_HASH="${BRIDGE_GIT_HASH}")

# Set in the untracked local.cmake, see the top level CMakeLists.txt.
if(BRIDGE_STA_SSID)
    target_compile_definitions(${COMPONENT_LIB}
                               PRIVATE BRIDGE_STA_SSID="${BRIDGE_STA_SSID}"
                                       BRIDGE_STA_PASSWORD="${BRIDGE_STA_PASSWORD}")
endif()
