# Replay variant: sensor frames come from a blackbox file, paced by the
# recorded timestamps. Open loop, stdio only, desktop only.
add_library(platform_replay STATIC
    sensor_source_replay.cpp)
target_include_directories(platform_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The blackbox record layout comes from protocol/blackbox.hpp: no link to
# flight_core itself (the impl libs must not depend on it).
target_link_libraries(platform_replay
    PUBLIC platform
    PRIVATE protocol drone_warnings drone_strict)
