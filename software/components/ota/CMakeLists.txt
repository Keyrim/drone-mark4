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

# The updater on the wire: one Ota* request in, at most one reply out, to
# the node that asked. Header-only like the rest, over the messenger.
add_library(ota_provider INTERFACE)
target_include_directories(ota_provider INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota_provider INTERFACE ota messaging)

# The gate of a node that flies: the flight core answers for the motors.
# Separate because a node that does not fly (the relay) writes its own
# two-line gate and links nothing of flight-core.
add_library(ota_flight_gate INTERFACE)
target_include_directories(ota_flight_gate INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota_flight_gate INTERFACE ota_provider flight_core)
