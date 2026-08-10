# Frame budget baked in at build time: 0 = no limit. A finite budget is a
# bench or CI decision, so it is a compile switch, never a runtime argument.
set(DRONE_SIM_FRAME_LIMIT 0 CACHE STRING "Frames drone_sim processes before stopping, 0 = no limit")

add_executable(drone_sim
    drone_sim_app.cpp
    main.cpp)
target_compile_definitions(drone_sim PRIVATE DRONE_SIM_FRAME_LIMIT=${DRONE_SIM_FRAME_LIMIT}U)
# drone_strict: same regime as the libs (-fno-rtti) - also required so that
# UBSan's vptr check does not reference typeinfo that is never emitted.
target_link_libraries(
    drone_sim PRIVATE flight_core platform_common platform_sim protocol drone_warnings drone_strict)
