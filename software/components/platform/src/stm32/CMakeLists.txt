# Target variant: startup + vector table, board clock/timebase/LEDs, RTT
# debug console, the I2C master the sensor drivers sit on, and the internal
# flash driver the update system stores firmware slots in.
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
    sensor_source_stm32.cpp
    internal_flash.cpp
    ota_meta_flash.cpp
    firmware_store_stm32.cpp)
target_include_directories(platform_stm32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# protocol is public: command_receiver_stm32.hpp exposes the framing parser.
# platform_common is public too: the firmware store is an OtaMetaLog over a
# flash backend, and drone_boot reuses the same log.
target_link_libraries(platform_stm32
    PUBLIC platform platform_common protocol
    PRIVATE drone_warnings drone_strict)

# The flash map of docs/ota-design.md section 4.2, mirrored in C++ by
# platform_stm32/ota_slots.hpp: change the two together. The bootloader owns
# sectors 0-1, each firmware slot three 128 KB sectors, and a slot image
# opens with the OTA image header so its vector table lands 512-byte aligned.
set(DRONE_STM32_BOOT_ORIGIN 0x08000000 CACHE INTERNAL "drone_boot flash base")
set(DRONE_STM32_BOOT_LENGTH 32K CACHE INTERNAL "drone_boot flash window")
set(DRONE_STM32_SLOT_A_ORIGIN 0x08020000 CACHE INTERNAL "firmware slot A flash base")
set(DRONE_STM32_SLOT_B_ORIGIN 0x08080000 CACHE INTERNAL "firmware slot B flash base")
set(DRONE_STM32_SLOT_LENGTH 384K CACHE INTERNAL "firmware slot flash window")
set(DRONE_STM32_IMAGE_HEADER_SIZE 512 CACHE INTERNAL "OTA_IMAGE_HEADER_SIZE of protocol/ota.hpp")
set(DRONE_STM32_LD_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/stm32f405_image.ld.in"
    CACHE INTERNAL "STM32F405 image layout template")

# Generates one linker script for one executable from the single template,
# and attaches it to that target. Every image on this chip therefore shares
# its section rules and differs only by the three numbers passed here; there
# is no per-image copy of the layout to drift.
#
#   drone_stm32_image_layout(<target>
#       FLASH_ORIGIN <address> FLASH_LENGTH <size> IMAGE_HEADER_SIZE <bytes>)
#
# IMAGE_HEADER_SIZE is 0 for an image that carries no OTA header (drone_boot).
function(drone_stm32_image_layout target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "FLASH_ORIGIN;FLASH_LENGTH;IMAGE_HEADER_SIZE" "")
    set(DRONE_IMAGE_NAME "${target}")
    set(DRONE_FLASH_ORIGIN "${ARG_FLASH_ORIGIN}")
    set(DRONE_FLASH_LENGTH "${ARG_FLASH_LENGTH}")
    set(DRONE_IMAGE_HEADER_SIZE "${ARG_IMAGE_HEADER_SIZE}")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/${target}.ld")
    configure_file("${DRONE_STM32_LD_TEMPLATE}" "${script}" @ONLY)
    target_link_options(${target} PRIVATE "-T${script}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${script}")
endfunction()
