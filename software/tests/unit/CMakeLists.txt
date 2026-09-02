include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    # v3.8.1, pinned by commit: a tag can be force-pushed and would change
    # what CI builds with no diff in this repo.
    GIT_TAG 2b60af89e23d28eefc081bc930831ee9d45ea58b)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests
    test_attitude_estimator.cpp
    test_command_receiver.cpp
    test_control.cpp
    test_flight_core.cpp
    test_hub_http.cpp
    test_hub_gateway.cpp
    test_hub_ota.cpp
    test_hub_tuning_profiles.cpp
    test_log.cpp
    test_motor_sink_dshot.cpp
    test_ota_boot_policy.cpp
    test_ota_e2e.cpp
    test_ota_meta_log.cpp
    test_ota_store_sim.cpp
    test_ota_updater.cpp
    test_pilot_modes.cpp
    test_platform_sim_link.cpp
    test_protocol_envelope.cpp
    test_plant_link.cpp
    test_rc_tracker.cpp
    test_serial_framing.cpp
    test_sim_run_tracker.cpp
    test_status.cpp
    test_throw_detector.cpp
    test_tuning.cpp
    test_transport.cpp
    test_tuning_service.cpp
    test_vertical_estimator.cpp)
target_link_libraries(unit_tests
    PRIVATE flight_core platform_common platform_sim protocol transport log log_wire hub_core
            Catch2::Catch2WithMain drone_warnings)
# The libs under test are compiled with -fno-rtti (drone_strict) and emit no
# typeinfo, so UBSan's vptr check cannot be applied to their classes from here
# (it would reference symbols that are never emitted). Nothing is lost: those
# libs are not instrumented by that check in the first place. RTTI and
# exceptions stay enabled in the tests, Catch2 needs them.
target_compile_options(unit_tests PRIVATE -fno-sanitize=vptr)

# The DShot frame encoding is a pure header in the stm32 variant (register-
# free), tested here without a board; reach its include dir directly since
# platform_stm32 itself only builds for the target.
target_include_directories(unit_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/components/platform/src/stm32/include)

# Recorded sessions the tests read: too large to spell out in a source file,
# and their exact values are the point.
target_compile_definitions(unit_tests
    PRIVATE MARK4_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The plant link test spawns a headless Godot when one is on the PATH and
# skips otherwise; the project path is what it needs.
find_program(DRONE_GODOT godot)
target_compile_definitions(unit_tests
    PRIVATE MARK4_GODOT_PROJECT="${DRONE_REPO_ROOT}/sim-godot"
            MARK4_GODOT_BINARY="${DRONE_GODOT}")
# The GDScript transport has no C++ counterpart to spawn it from: its smoke
# test is a headless Godot script, run by ctest when a godot binary exists.
if(DRONE_GODOT)
    add_test(NAME godot_transport_check
        COMMAND ${DRONE_GODOT} --headless --path ${DRONE_REPO_ROOT}/sim-godot
                --script res://tests/transport_check.gd)
endif()

# The OTA end-to-end test spawns the real flight process and talks to it over
# real UDP, so it needs the binary itself, not a library: the path comes from
# the build tree and the dependency makes ctest impossible to run against a
# stale one.
target_compile_definitions(unit_tests PRIVATE DRONE_SIM_BINARY="$<TARGET_FILE:drone_sim>")
add_dependencies(unit_tests drone_sim)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(unit_tests)
