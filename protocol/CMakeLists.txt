# Header-only INTERFACE lib, zero dependency: included by flight-core, the
# apps, the ESP32, and parsed by Godot and the ground station.
add_library(protocol INTERFACE)
target_include_directories(protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
