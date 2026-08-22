# The firmware is linked once per OTA slot: same sources, same objects, one
# -DDRONE_OTA_SLOT_ID and one flash window each. The slot id reaches the
# image header (image_header.cpp) and the composition root's firmware store
# through platform_stm32/ota_slots.hpp, so nothing else in the tree has to
# know that two variants exist.
set(DRONE_FIRMWARE_SOURCES
    main.cpp
    firmware_app.cpp
    status_leds.cpp
    image_header.cpp)

# name -> (slot id, flash origin). OTA_SLOT_A / OTA_SLOT_B of protocol/ota.hpp.
set(DRONE_FIRMWARE_A_SLOT_ID 0)
set(DRONE_FIRMWARE_B_SLOT_ID 1)
set(DRONE_FIRMWARE_A_ORIGIN ${DRONE_STM32_SLOT_A_ORIGIN})
set(DRONE_FIRMWARE_B_ORIGIN ${DRONE_STM32_SLOT_B_ORIGIN})

foreach(variant IN ITEMS A B)
    string(TOLOWER ${variant} suffix)
    set(target drone_firmware_${suffix})
    add_executable(${target} ${DRONE_FIRMWARE_SOURCES})
    set_target_properties(${target} PROPERTIES SUFFIX ".elf")
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target}
        PRIVATE DRONE_OTA_SLOT_ID=${DRONE_FIRMWARE_${variant}_SLOT_ID})
    # drone_strict: the no-exceptions/no-RTTI invariant must not depend on the
    # toolchain file happening to duplicate the flags.
    target_link_libraries(${target}
        PRIVATE flight_core platform_common platform_stm32 protocol drone_warnings drone_strict)
    drone_stm32_image_layout(${target}
        FLASH_ORIGIN ${DRONE_FIRMWARE_${variant}_ORIGIN}
        FLASH_LENGTH ${DRONE_STM32_SLOT_LENGTH}
        IMAGE_HEADER_SIZE ${DRONE_STM32_IMAGE_HEADER_SIZE})
    if(DRONE_ARM_SIZE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${DRONE_ARM_SIZE} $<TARGET_FILE:${target}>)
    endif()
endforeach()

# `drone_firmware` is the app of software/apps.json: the two slot images plus
# the packaging step, so one build command ends with the flashable artifacts
# (slot_a.img, slot_b.img and the drone_firmware.ota bundle) next to the elfs.
#
# The packaging step runs on every build rather than tracking outputs,
# because one of its inputs is the git hash: a commit changes what the
# manifest must say without changing a single byte of either elf.
if(DRONE_PYTHON3 AND DRONE_ARM_OBJCOPY)
    add_custom_target(drone_firmware ALL
        COMMAND ${DRONE_PYTHON3} ${DRONE_REPO_ROOT}/scripts/make_ota.py
                --slot-a $<TARGET_FILE:drone_firmware_a>
                --slot-b $<TARGET_FILE:drone_firmware_b>
                --objcopy ${DRONE_ARM_OBJCOPY}
                --outdir ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS drone_firmware_a drone_firmware_b ${DRONE_REPO_ROOT}/scripts/make_ota.py
        COMMENT "packaging drone_firmware.ota"
        VERBATIM)
else()
    message(STATUS "make_ota.py needs python3 and arm-none-eabi-objcopy: "
                   "'drone_firmware' builds the slot images only")
    add_custom_target(drone_firmware ALL DEPENDS drone_firmware_a drone_firmware_b)
endif()
