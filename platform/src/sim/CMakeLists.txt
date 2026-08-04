# Network-less stubs: frames generated internally at a fixed rate, enough to
# prove the service composition. UDP transports come later.
add_library(platform_sim STATIC
    clock_sim.cpp
    sensor_source_sim.cpp
    motor_sink_sim.cpp
    telemetry_sender_sim.cpp)
target_include_directories(platform_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(platform_sim
    PUBLIC platform
    PRIVATE drone_warnings drone_strict)
