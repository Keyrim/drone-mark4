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

# No TLS and no compression: the endpoint is local, and both options would
# pull a system library into a build that needs none.
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    # v11.4.6, pinned by commit: a tag can be force-pushed and would change
    # what CI builds with no diff in this repo.
    GIT_TAG 2efe037c9cc96fd536774f17bdb5215161ee5087
    SYSTEM)
FetchContent_MakeAvailable(ixwebsocket)

# Everything the hub does that a unit test can exercise without a socket, a
# thread or a child process. This is the target the tests link.
add_library(hub_core STATIC
    src/discovery.cpp
    src/http_api.cpp
    src/json_codec.cpp
    src/stream_recorder.cpp
    src/tuning_profiles.cpp)
target_include_directories(hub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hub_core PUBLIC protocol nlohmann_json::nlohmann_json)
target_link_libraries(hub_core PRIVATE drone_warnings)

# The hub itself: hub_core plus everything that owns a resource. Desktop only,
# and deliberately not linked against flight_core or platform - the hub speaks
# protocol/ and nothing else.
add_executable(hub
    src/hub_app.cpp
    src/launcher.cpp
    src/main.cpp
    src/serial_transport.cpp
    src/udp_transport.cpp
    src/ws_bridge.cpp)
target_link_libraries(hub PRIVATE hub_core protocol ixwebsocket drone_warnings)
