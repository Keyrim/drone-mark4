# `platform` = the public interfaces alone (INTERFACE target, linked by
# flight-core). Implementation libs are only declared in the presets where
# they make sense, and are linked by the apps.
add_library(platform INTERFACE)
target_include_directories(platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(platform INTERFACE flight_core_types)

if(CMAKE_CROSSCOMPILING)
    add_subdirectory(src/stm32)
else()
    add_subdirectory(src/sim)
endif()
