# Desktop variant: sensor frames come in over UDP from the simulator, actuator
# frames go back to it, telemetry is broadcast. POSIX sockets and stdio,
# desktop only.
add_library(platform_sim STATIC
    clock_sim.cpp
    udp_socket.cpp
    sensor_source_sim.cpp
    motor_sink_sim.cpp
    firmware_store_sim.cpp)
target_include_directories(platform_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# protocol and platform_common are PUBLIC: the sensor source and the motor
# sink expose wire structs, the run tracker is a header of this library and
# sends through the shared envelope helper.
target_link_libraries(platform_sim
    PUBLIC platform protocol platform_common
    PRIVATE drone_warnings drone_strict)
