# Tuning: the parameter table of the flight core on the wire. Header-only,
# the glue between the tuning messages of protocol/mark4.proto and the
# registry the core owns. No heap, no iostream, no exceptions: it builds
# for the F405 as it stands.
add_library(tuning_provider INTERFACE)
target_include_directories(tuning_provider INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tuning_provider INTERFACE messaging flight_core log)

# The other side of the same concept: the parameter table of a node as a
# ground node holds it, pulled page by page, plus set and get. Header-only,
# fixed tables sized by the composition, no heap.
add_library(tuning_consumer INTERFACE)
target_include_directories(tuning_consumer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tuning_consumer INTERFACE messaging discovery log)
