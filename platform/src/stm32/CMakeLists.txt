# Target variant: startup + vector table, board clock/timebase/LEDs, RTT
# debug console and the I2C master the sensor drivers will sit on.
add_library(platform_stm32 STATIC
    startup.c
    board.cpp
    rtt.cpp
    i2c_bus.cpp
    mpu6050.cpp
    ms5611.cpp
    uart1.cpp
    telemetry_sender_stm32.cpp
    command_receiver_stm32.cpp
    clock_stm32.cpp
    sensor_source_stm32.cpp)
target_include_directories(platform_stm32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# protocol is public: command_receiver_stm32.hpp exposes the framing parser.
target_link_libraries(platform_stm32
    PUBLIC platform protocol
    PRIVATE drone_warnings drone_strict)

# The linker script describes the chip: it belongs to the stm32 variant and
# propagates to the executables that link it.
target_link_options(platform_stm32 INTERFACE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/stm32f405.ld")
set_target_properties(platform_stm32 PROPERTIES
    INTERFACE_LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32f405.ld")
