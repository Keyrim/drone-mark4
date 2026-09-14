# The wire services of a flight composition: telemetry, tuning and the
# updater, each a handler of the messenger that answers the node that asked.
# Header-only: the glue between the messages of protocol/mark4.proto and the
# objects that own the answers (the telemetry registry, the flight core's
# parameter table, the OtaUpdater). No heap, no iostream, no exceptions: it
# builds for the F405 as it stands.
add_library(services INTERFACE)
target_include_directories(services INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(services INTERFACE messaging flight_core telemetry log ota)
