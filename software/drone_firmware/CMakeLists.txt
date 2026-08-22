add_executable(drone_firmware
    main.cpp
    firmware_app.cpp
    status_leds.cpp)
set_target_properties(drone_firmware PROPERTIES SUFFIX ".elf")
# drone_strict: the no-exceptions/no-RTTI invariant must not depend on the
# toolchain file happening to duplicate the flags.
target_link_libraries(
    drone_firmware PRIVATE flight_core platform_common platform_stm32 protocol drone_warnings drone_strict)

find_program(DRONE_ARM_SIZE arm-none-eabi-size)
if(DRONE_ARM_SIZE)
    add_custom_command(TARGET drone_firmware POST_BUILD
        COMMAND ${DRONE_ARM_SIZE} $<TARGET_FILE:drone_firmware>)
endif()
