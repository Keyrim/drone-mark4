# Golden packet fixtures: the C++ structs are the source of truth, the
# committed .bin files are their frozen output, and every hand-written
# parser (python, GDScript) must decode those exact bytes in CI. The
# ctest below regenerates the bytes and compares them to the committed
# files, so any C++ layout change forces a conscious fixture update.
add_executable(golden_packets golden_packets.cpp)
# platform_common only for the CRC-32/MPEG-2 the OTA image header carries:
# the fixture stamps a real checksum, so the python side must reproduce the
# exact algorithm the board verifies images with.
target_link_libraries(golden_packets PRIVATE protocol platform_common transport drone_warnings)

add_test(NAME "golden packets match the committed fixtures"
         COMMAND golden_packets --check ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
