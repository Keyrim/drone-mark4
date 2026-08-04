include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests
    test_flight_core.cpp)
target_link_libraries(unit_tests PRIVATE flight_core Catch2::Catch2WithMain drone_warnings)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(unit_tests)
