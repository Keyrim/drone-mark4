# The postman and the sender: the one place an Envelope meets the transport,
# in both directions. One handler per body tag, a table indexed by tag, an
# optional raw tap, poll() over the transport's poll, send() as the one
# encoder. No heap, no iostream, no exceptions: it builds for the F405 as it
# stands. Links transport (the frames) and protocol (the codec), the two it
# joins; no composition links it yet.
add_library(messaging STATIC src/messenger.cpp)
target_include_directories(messaging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(messaging PUBLIC transport protocol PRIVATE drone_warnings drone_strict)
