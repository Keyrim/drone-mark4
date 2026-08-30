# The logging library of every node: modules registered by static objects,
# a level per module settable at runtime, printf formatting into a fixed
# buffer, up to two sinks. `log` is a leaf (drone_warnings alone) and builds
# on every preset; `log_wire` adds what the wire needs (the transport sink,
# the LogModules table, LogControl) and therefore links protocol/. The
# console sink is POSIX stdio, desktop only; the RTT sink lives with the
# stm32 platform variant, next to the RTT driver it writes through.
add_library(log STATIC src/module.cpp)
target_include_directories(log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(log PRIVATE drone_warnings drone_strict)

add_library(log_wire STATIC src/wire.cpp)
target_link_libraries(log_wire PUBLIC log protocol PRIVATE drone_warnings drone_strict)

if(DRONE_PLATFORM STREQUAL "desktop")
    target_sources(log PRIVATE src/posix/console_sink_posix.cpp)
endif()
