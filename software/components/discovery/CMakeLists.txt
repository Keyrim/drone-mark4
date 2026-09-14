# Who is who on the wire. Discovery answers an IdentityRequest with the
# node's Announce, unicast to whoever asked; DiscoveryDirectory is the same
# handler for a node that also needs to know who is around: it asks every
# node the transport hears, retries on a timeout, gives up after a few, and
# keeps the answers by kind for the composition and its listeners. No heap,
# no iostream, no exceptions: it builds for the F405 as it stands. Links
# messaging alone; the transport and the codec come with it.
add_library(discovery STATIC src/discovery.cpp src/discovery_directory.cpp)
target_include_directories(discovery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(discovery PUBLIC messaging PRIVATE drone_warnings drone_strict)
