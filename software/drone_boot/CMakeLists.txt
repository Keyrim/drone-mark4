# The bootloader: sectors 0-1, SWD-flashed once per board, never updated
# over the air. It links platform_stm32 for the startup file, the LEDs and
# the internal flash driver, and platform_common for OtaMetaLog - the one
# metadata implementation it shares with the firmware store, so the two can
# never read the log differently. flight_core arrives on the link line
# through platform_common but contributes no object: nothing here references
# it, and that is deliberate.
add_executable(drone_boot
    main.cpp
    boot_app.cpp)
set_target_properties(drone_boot PROPERTIES SUFFIX ".elf")
target_link_libraries(drone_boot
    PRIVATE platform_common platform_stm32 protocol drone_warnings drone_strict)

drone_stm32_image_layout(drone_boot
    FLASH_ORIGIN ${DRONE_STM32_BOOT_ORIGIN}
    FLASH_LENGTH ${DRONE_STM32_BOOT_LENGTH}
    IMAGE_HEADER_SIZE 0)

if(DRONE_ARM_SIZE)
    add_custom_command(TARGET drone_boot POST_BUILD
        COMMAND ${DRONE_ARM_SIZE} $<TARGET_FILE:drone_boot>)
endif()

# The flash script needs a raw binary at 0x08000000; the elf is what the
# debugger uses.
if(DRONE_ARM_OBJCOPY)
    add_custom_command(TARGET drone_boot POST_BUILD
        COMMAND ${DRONE_ARM_OBJCOPY} -O binary
                $<TARGET_FILE:drone_boot> ${CMAKE_CURRENT_BINARY_DIR}/drone_boot.bin
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/drone_boot.bin
        COMMENT "drone_boot.bin (flash at 0x08000000)")
endif()
