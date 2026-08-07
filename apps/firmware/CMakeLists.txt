add_executable(firmware
    main.cpp
    firmware_app.cpp)
set_target_properties(firmware PROPERTIES SUFFIX ".elf")
target_link_libraries(firmware PRIVATE flight_core platform_stm32 drone_warnings)

find_program(DRONE_ARM_SIZE arm-none-eabi-size)
if(DRONE_ARM_SIZE)
    add_custom_command(TARGET firmware POST_BUILD
        COMMAND ${DRONE_ARM_SIZE} $<TARGET_FILE:firmware>)
endif()
