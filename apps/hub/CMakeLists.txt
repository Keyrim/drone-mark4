include(FetchContent)

# SYSTEM: the fetched headers are not project code, so -Werror must not turn
# their warnings into build failures.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    # v3.12.0, pinned by commit: a tag can be force-pushed and would change
    # what CI builds with no diff in this repo.
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72
    SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

# Everything the hub does that a unit test can exercise without a socket, a
# thread or a child process. This is the target the tests link.
add_library(hub_core STATIC
    src/discovery.cpp
    src/json_codec.cpp
    src/stream_recorder.cpp)
target_include_directories(hub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hub_core PUBLIC protocol nlohmann_json::nlohmann_json)
target_link_libraries(hub_core PRIVATE drone_warnings)

# The hub itself: hub_core plus everything that owns a resource. Desktop only,
# and deliberately not linked against flight_core or platform - the hub speaks
# protocol/ and nothing else.
add_executable(hub
    src/hub_app.cpp
    src/main.cpp
    src/serial_transport.cpp
    src/udp_transport.cpp)
target_link_libraries(hub PRIVATE hub_core protocol drone_warnings)
