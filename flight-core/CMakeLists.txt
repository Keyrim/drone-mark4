# Public types (SensorFrame/ActuatorFrame) are exposed on their own so the
# platform interfaces can reference them without depending on the lib.
add_library(flight_core_types INTERFACE)
target_include_directories(flight_core_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Pure static library: no dynamic allocation, no exceptions, no RTTI,
# no clock access, no waiting.
add_library(flight_core STATIC
    src/blackbox.cpp
    src/estimator/attitude_estimator.cpp
    src/estimator/vertical_estimator.cpp
    src/flight_core.cpp
    src/telemetry.cpp
    src/throw_launch/throw_detector.cpp)
target_link_libraries(flight_core
    PUBLIC flight_core_types platform protocol
    PRIVATE drone_warnings drone_strict)
