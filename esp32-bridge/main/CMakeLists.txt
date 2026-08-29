# The relay: the WiFi bring-up (C, bridge_main.c), the relay itself
# (C++, relay.cpp) and the shared transport sources it is built on, compiled
# here as they are (no copy): the core, the UART link and the BSD-socket UDP
# link, which lwIP serves like POSIX does. The desktop-only node_id.cpp
# (/dev/urandom) stays out.
set(DRONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../software/components")
set(DRONE_TRANSPORT "${DRONE_ROOT}/transport")

idf_component_register(SRCS "bridge_main.c"
                            "relay.cpp"
                            "${DRONE_TRANSPORT}/src/transport.cpp"
                            "${DRONE_TRANSPORT}/src/uart_link.cpp"
                            "${DRONE_TRANSPORT}/src/posix/udp_link.cpp"
                       INCLUDE_DIRS "." "${DRONE_TRANSPORT}/include")

# The same warning set as the desktop and STM32 builds (drone_warnings), and
# the same freestanding C++ as the shared libraries (drone_strict).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wconversion -Wdouble-promotion -Werror
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# Set in the untracked local.cmake, see the top level CMakeLists.txt.
if(BRIDGE_STA_SSID)
    target_compile_definitions(${COMPONENT_LIB}
                               PRIVATE BRIDGE_STA_SSID="${BRIDGE_STA_SSID}"
                                       BRIDGE_STA_PASSWORD="${BRIDGE_STA_PASSWORD}")
endif()
