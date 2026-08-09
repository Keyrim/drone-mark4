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
    test_blackbox.cpp
    test_control.cpp
    test_flight_core.cpp
    test_hub_discovery.cpp
    test_hub_json.cpp
    test_platform_sim_udp.cpp
    test_replay.cpp
    test_serial_framing.cpp
    test_telemetry.cpp
    test_throw_detector.cpp
    test_vertical_estimator.cpp)
target_link_libraries(unit_tests
    PRIVATE flight_core platform_common platform_sim platform_replay protocol hub_core
            Catch2::Catch2WithMain drone_warnings)
# The libs under test are compiled with -fno-rtti (drone_strict) and emit no
# typeinfo, so UBSan's vptr check cannot be applied to their classes from here
# (it would reference symbols that are never emitted). Nothing is lost: those
# libs are not instrumented by that check in the first place. RTTI and
# exceptions stay enabled in the tests, Catch2 needs them.
target_compile_options(unit_tests PRIVATE -fno-sanitize=vptr)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(unit_tests)
