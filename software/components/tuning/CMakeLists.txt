# Tuning: the parameter table of the flight core on the wire. Header-only,
# the glue between the tuning messages of protocol/mark4.proto and the
# registry the core owns. No heap, no iostream, no exceptions: it builds
# for the F405 as it stands.
add_library(tuning_provider INTERFACE)
target_include_directories(tuning_provider INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tuning_provider INTERFACE messaging flight_core log)
