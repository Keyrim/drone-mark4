# Interface manager between the processes and boards of the project: frames
# an opaque payload with (src, dst, seq, hops), learns where every node was
# last heard and emits on the matching physical link. Depends on nothing but
# the warning set: protocol/ is payload to it, never a dependency.
#
# The core and the UART link build for every preset, the F405 included; the
# socket link needs BSD sockets: desktop here, lwIP in esp32-bridge/.
add_library(transport STATIC
    src/transport.cpp
    src/uart_link.cpp)
target_include_directories(transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(transport PRIVATE drone_warnings drone_strict)

if(DRONE_PLATFORM STREQUAL "desktop")
    target_sources(transport PRIVATE
        src/posix/udp_link.cpp
        src/posix/node_id.cpp)
endif()
