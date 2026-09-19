# The firmware update brick every node with two firmware slots builds on:
# the AbsFirmwareStore interface and the boot metadata types, the OtaUpdater
# session state machine, the boot policy the bootloader and the desktop
# flight process share, the metadata log and the CRC-32/MPEG-2 helper.
# Header-only, protocol/ alone underneath: no heap, no iostream, no
# exceptions, so it builds for the F405, the ESP32 relay (which compiles
# software/components/ sources as they are) and the desktop alike. The
# store implementations stay with their targets (platform_stm32,
# platform_sim, esp32-bridge/main).
add_library(ota STATIC
    src/firmware_store.cpp
    src/boot_policy.cpp
    src/crc32_mpeg2.cpp
    src/meta_log.cpp
    src/image_header.cpp
    src/updater.cpp)
target_include_directories(ota PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota PUBLIC protocol
    PRIVATE drone_warnings drone_strict)

# The updater on the wire: one Ota* request in, at most one reply out, to
# the node that asked. Header-only like the rest, over the messenger.
add_library(ota_provider STATIC src/provider.cpp src/gate.cpp)
target_include_directories(ota_provider PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota_provider PUBLIC ota messaging
    PRIVATE drone_warnings drone_strict)

# The gate of a node that flies: the flight core answers for the motors.
# Separate because a node that does not fly (the relay) writes its own
# two-line gate and links nothing of flight-core.
add_library(ota_flight_gate STATIC src/flight_gate.cpp)
target_include_directories(ota_flight_gate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ota_flight_gate PUBLIC ota_provider flight_core
    PRIVATE drone_warnings drone_strict)

if(DRONE_PLATFORM STREQUAL "desktop")
    # The ground side of the same concept, and the one exemption to the rules
    # above: it reads a bundle off a filesystem and keeps the std::string and
    # std::function it was written with, so it is a static library declared
    # only where the desktop platform is configured, like platform_sim.
    # SYSTEM: the fetched headers are not project code, so -Werror must not
    # turn their warnings into build failures.
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        # v3.12.0, pinned by commit: a tag can be force-pushed and would change
        # what CI builds with no diff in this repo.
        GIT_TAG 55f93686c01528224f448c19128836e7df245f72
        SYSTEM)
    FetchContent_MakeAvailable(nlohmann_json)

    add_library(ota_consumer STATIC src/bundle.cpp src/consumer.cpp)
    target_include_directories(ota_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(ota_consumer
        PUBLIC ota protocol log
        PRIVATE nlohmann_json::nlohmann_json drone_warnings)
endif()
