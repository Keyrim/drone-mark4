# Desktop variant: sensor frames come in over UDP from the simulator, actuator
# frames go back to it, telemetry is broadcast. POSIX sockets and stdio,
# desktop only.
add_library(platform_sim STATIC
    clock_sim.cpp
    udp_link.cpp
    sensor_source_sim.cpp
    command_receiver_sim.cpp
    motor_sink_sim.cpp
    telemetry_sender_sim.cpp
    firmware_store_sim.cpp)
target_include_directories(platform_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# protocol is PUBLIC: the run tracker is a header of this library and carries
# the wire struct it broadcasts. platform_common is PRIVATE: the firmware
# store uses the shared metadata log inside its own translation unit only.
target_link_libraries(platform_sim
    PUBLIC platform protocol
    PRIVATE platform_common drone_warnings drone_strict)
