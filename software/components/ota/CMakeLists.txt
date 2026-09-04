# The firmware update brick every node with two firmware slots builds on:
# the AbsFirmwareStore interface and the boot metadata types, the OtaUpdater
# session state machine, the boot policy the bootloader and the desktop
# flight process share, the metadata log and the CRC-32/MPEG-2 helper.
# Header-only, protocol/ alone underneath: no heap, no iostream, no
# exceptions, so it builds for the F405, the ESP32 relay (which compiles
# software/components/ sources as they are) and the desktop alike. The
# store implementations stay with their targets (platform_stm32,
# platform_sim, esp32-bridge/main).
add_library(ota INTERFACE)
target_include_directories(ota INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota INTERFACE protocol)
