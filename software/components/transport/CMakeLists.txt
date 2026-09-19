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

# The transport report on the wire: what this node's transport and its
# messenger count, answered to the nodes that subscribed, one page of peers
# at a time. Header-only, no heap: it builds for the F405 as it stands.
add_library(transport_provider STATIC src/provider.cpp)
target_include_directories(transport_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(transport_provider
    PUBLIC messaging log
    PRIVATE drone_warnings drone_strict)

# The other side of the same concept: every node's view of the transport as
# a ground node holds it, the pages of one report merged into the peer table
# the reporting node holds. Header-only, fixed tables sized by the
# composition, no heap.
add_library(transport_consumer STATIC src/consumer.cpp)
target_include_directories(transport_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(transport_consumer
    PUBLIC messaging discovery log
    PRIVATE drone_warnings drone_strict)
