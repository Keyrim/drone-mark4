include(FetchContent)

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
    src/gateway_codec.cpp
    src/http_api.cpp
    src/tuning_profiles.cpp)
target_include_directories(hub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_SOURCE_DIR})
# nlohmann_json reads the on-disk formats (the profile files, the saved
# telemetry views); nothing of it reaches the websocket. It is fetched by
# components/ota, which reads the bundle manifest with it. ota is
# header-only and brings the one CRC of the update system, the same code the
# boards run.
target_link_libraries(hub_core
    PUBLIC protocol ota ota_consumer nanopb_gateway transport log log_wire
           nlohmann_json::nlohmann_json)
target_link_libraries(hub_core PRIVATE drone_warnings)

# The hub itself: hub_core plus everything that owns a resource. Desktop only,
# and deliberately not linked against flight_core or platform - the hub speaks
# protocol/ and nothing else.
add_executable(hub
    src/hub_app.cpp
    src/main.cpp
    src/ws_bridge.cpp)
target_link_libraries(hub
    PRIVATE hub_core protocol transport messaging discovery log_provider log_consumer
            status_consumer telemetry_consumer tuning_consumer ixwebsocket drone_warnings)
# The transport is compiled with -fno-rtti (drone_strict) and emits no
# typeinfo, so UBSan's vptr check cannot be applied to its classes from
# here: it would reference symbols that are never emitted. Nothing is lost,
# the transport is not instrumented by that check in the first place.
target_compile_options(hub PRIVATE -fno-sanitize=vptr)
