# Status: the small fixed report of what a drone is doing, packed from the
# flight core state and streamed to the nodes that subscribed. Header-only,
# the glue between the accessors of flight-core and the Status message of
# protocol/mark4.proto. No heap, no iostream, no exceptions: it builds for
# the F405 as it stands.
add_library(status_provider STATIC src/status_provider.cpp src/status_packer.cpp)
target_include_directories(status_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(status_provider
    PUBLIC messaging flight_core log
    PRIVATE drone_warnings drone_strict)

# The other side of the same concept: what a ground node keeps of a drone's
# Status, one subscription and one last report per node. Header-only, fixed
# tables sized by the composition, no heap.
add_library(status_consumer STATIC src/consumer.cpp)
target_include_directories(status_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(status_consumer
    PUBLIC messaging discovery log
    PRIVATE drone_warnings drone_strict)
