# The wire, for the relay: the nanopb runtime and the codec generated from
# the one schema of the project, software/components/protocol/mark4.proto,
# by the same generator invocation as software/components/protocol/
# CMakeLists.txt (same nanopb commit, same options, same PB_NO_MALLOC /
# PB_BUFFER_ONLY). The relay only reads one tag byte of every envelope
# (protocol/envelope.hpp, envelopeIsAnnounce), the codec is here so a
# future look inside a payload is a call away, not a build change.
# ESP-IDF reads every component file once in script mode to collect its
# requirements; nothing below can run there, and this component needs none.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register()
    return()
endif()

include(FetchContent)

set(DRONE_PROTOCOL "${CMAKE_CURRENT_SOURCE_DIR}/../../../software/components/protocol")
set(DRONE_PROTO_FILE "${DRONE_PROTOCOL}/mark4.proto")
set(DRONE_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")

# nanopb_generator.py needs the python packages protobuf and grpcio-tools.
# The ESP-IDF python environment does not ship them; the system python of
# the dev image does (the desktop build needs them too), so the first
# interpreter that imports both is the one used.
foreach(candidate IN ITEMS "${PYTHON}" "/usr/bin/python3" "python3")
    if(candidate STREQUAL "")
        continue()
    endif()
    execute_process(
        COMMAND ${candidate} -c "import grpc_tools.protoc, google.protobuf"
        RESULT_VARIABLE DRONE_PROTO_PYTHON_OK
        OUTPUT_QUIET ERROR_QUIET)
    if(DRONE_PROTO_PYTHON_OK EQUAL 0)
        set(DRONE_PROTO_PYTHON "${candidate}")
        break()
    endif()
endforeach()
if(NOT DRONE_PROTO_PYTHON)
    message(FATAL_ERROR
        "the wire codec needs a python with protobuf and grpcio-tools: "
        "pip install --user protobuf grpcio-tools (the devcontainer image ships them)")
endif()

# SOURCE_SUBDIR names a directory nanopb does not have, so its own
# CMakeLists.txt is never processed: only the sources are wanted.
FetchContent_Declare(
    nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    # nanopb-0.4.9.2, pinned by commit, the same as the desktop build.
    GIT_TAG 4133c1d3631654e80ed9bd27139318fd79fc2528
    SOURCE_SUBDIR sources-only-no-cmake)
FetchContent_MakeAvailable(nanopb)

# The wire hash header the envelope helpers include, computed like the
# desktop build computes it: 8 hex characters of the schema's SHA-256.
# The generated tree must exist at configure time: ESP-IDF checks every
# include directory it is handed.
file(MAKE_DIRECTORY ${DRONE_PROTO_GEN_DIR}/nanopb)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DRONE_PROTO_FILE})
file(SHA256 ${DRONE_PROTO_FILE} DRONE_PROTO_SHA256)
string(SUBSTRING "${DRONE_PROTO_SHA256}" 0 8 DRONE_WIRE_HASH_HEX)
configure_file(${DRONE_PROTOCOL}/wire_hash.hpp.in
               ${DRONE_PROTO_GEN_DIR}/include/protocol/wire_hash.hpp @ONLY)

add_custom_command(
    OUTPUT ${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.c ${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DRONE_PROTO_GEN_DIR}/nanopb
    COMMAND ${DRONE_PROTO_PYTHON} ${nanopb_SOURCE_DIR}/generator/nanopb_generator.py -q
            -I ${DRONE_PROTOCOL} -D ${DRONE_PROTO_GEN_DIR}/nanopb mark4.proto
    DEPENDS ${DRONE_PROTO_FILE} ${DRONE_PROTOCOL}/mark4.options
    COMMENT "generating the nanopb codec of mark4.proto"
    VERBATIM)
add_custom_target(mark4_proto_gen
                  DEPENDS ${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.c
                          ${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.h)
set_property(SOURCE ${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.c PROPERTY GENERATED 1)

idf_component_register(SRCS "${nanopb_SOURCE_DIR}/pb_common.c"
                            "${nanopb_SOURCE_DIR}/pb_encode.c"
                            "${nanopb_SOURCE_DIR}/pb_decode.c"
                            "${DRONE_PROTO_GEN_DIR}/nanopb/mark4.pb.c"
                       INCLUDE_DIRS "${nanopb_SOURCE_DIR}"
                                    "${DRONE_PROTO_GEN_DIR}/nanopb"
                                    "${DRONE_PROTO_GEN_DIR}/include"
                                    "${DRONE_PROTOCOL}/include")
add_dependencies(${COMPONENT_LIB} mark4_proto_gen)
target_compile_definitions(${COMPONENT_LIB} PUBLIC PB_NO_MALLOC PB_BUFFER_ONLY PB_NO_ERRMSG)
