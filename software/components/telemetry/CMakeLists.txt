# The telemetry registry: names, units and where values live. A leaf, like
# `log`: no heap, no iostream, no exceptions, no wire. It builds for the
# F405 as it stands, and flight-core links it because measures are declared
# next to the variables they read - a name and a pointer, not a message.
add_library(telemetry STATIC src/registry.cpp)
target_include_directories(telemetry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(telemetry PRIVATE drone_warnings drone_strict)

# The registry on the wire: the frozen table, the pages, the configuration
# and the sample stream. Header-only, no heap, no iostream, no exceptions:
# it builds for the F405 as it stands.
add_library(telemetry_provider INTERFACE)
target_include_directories(telemetry_provider INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(telemetry_provider INTERFACE telemetry messaging log)
