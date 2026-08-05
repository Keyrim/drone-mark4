# Desktop variant: sensor frames come in over UDP from the simulator, actuator
# frames go back to it, telemetry is broadcast, blackbox records go to a file.
# POSIX sockets and stdio, desktop only.
add_library(platform_sim STATIC
    clock_sim.cpp
    udp_link.cpp
    sensor_source_sim.cpp
    motor_sink_sim.cpp
    telemetry_sender_sim.cpp
    log_sink_file.cpp)
target_include_directories(platform_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(platform_sim
    PUBLIC platform
    PRIVATE protocol drone_warnings drone_strict)
