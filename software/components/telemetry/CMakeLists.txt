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
add_library(telemetry_provider STATIC src/provider.cpp)
target_include_directories(telemetry_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(telemetry_provider
    PUBLIC telemetry messaging log
    PRIVATE drone_warnings drone_strict)

# The other side of the same concept: another node's measure table as a
# ground node holds it, pulled page by page, plus the configuration and the
# sample stream. Header-only, fixed tables sized by the composition, no heap;
# it links telemetry for MAX_TELEMETRY_ENTRIES, the bound of one table.
add_library(telemetry_consumer STATIC src/consumer.cpp)
target_include_directories(telemetry_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(telemetry_consumer
    PUBLIC telemetry messaging discovery log
    PRIVATE drone_warnings drone_strict)
