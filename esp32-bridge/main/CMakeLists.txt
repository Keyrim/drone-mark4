idf_component_register(SRCS "bridge_main.c"
                       INCLUDE_DIRS ".")

# Set in the untracked local.cmake, see the top level CMakeLists.txt.
if(BRIDGE_STA_SSID)
    target_compile_definitions(${COMPONENT_LIB}
                               PRIVATE BRIDGE_STA_SSID="${BRIDGE_STA_SSID}"
                                       BRIDGE_STA_PASSWORD="${BRIDGE_STA_PASSWORD}")
endif()
