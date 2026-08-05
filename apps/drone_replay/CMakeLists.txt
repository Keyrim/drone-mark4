add_executable(drone_replay
    drone_replay_app.cpp
    main.cpp)
# drone_strict: same regime as the libs (-fno-rtti) - also required so that
# UBSan's vptr check does not reference typeinfo that is never emitted.
target_link_libraries(drone_replay
    PRIVATE flight_core platform_replay platform_sim protocol drone_warnings drone_strict)
