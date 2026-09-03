# Public types (SensorFrame/ActuatorFrame) are exposed on their own so the
# platform interfaces can reference them without depending on the lib.
add_library(flight_core_types INTERFACE)
target_include_directories(flight_core_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Pure static library: no dynamic allocation, no exceptions, no RTTI,
# no clock access, no waiting.
add_library(flight_core STATIC
    src/control/attitude_controller.cpp
    src/control/mixer.cpp
    src/control/rate_controller.cpp
    src/control/vertical_controller.cpp
    src/estimator/attitude_estimator.cpp
    src/estimator/vertical_estimator.cpp
    src/flight_core.cpp
    src/throw_launch/throw_detector.cpp
    src/tuning_table.cpp)
# Pure algorithms: no platform, no protocol, no wire. `telemetry` is not
# the wire: it is names, units and pointers, so a module can expose what it
# computes without knowing a message exists. The library rebuilds only when
# the algorithms change.
target_link_libraries(flight_core
    PUBLIC flight_core_types telemetry
    PRIVATE drone_warnings drone_strict)
