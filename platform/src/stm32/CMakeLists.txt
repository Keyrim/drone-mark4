# Minimal stub: startup + vector table, no real drivers yet.
# Proves that flight-core cross-compiles and links for Cortex-M4F.
add_library(platform_stm32 STATIC
    startup.c)
target_link_libraries(platform_stm32
    PUBLIC platform
    PRIVATE drone_warnings)

# The linker script describes the chip: it belongs to the stm32 variant and
# propagates to the executables that link it.
target_link_options(platform_stm32 INTERFACE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/stm32f405.ld")
set_target_properties(platform_stm32 PROPERTIES
    INTERFACE_LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stm32f405.ld")
