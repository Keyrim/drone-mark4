# `platform` = the public interfaces alone (INTERFACE target, linked by
# flight-core). Implementation libs are only declared in the presets where
# they make sense, and are linked by the apps.
add_library(platform INTERFACE)
target_include_directories(platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(platform INTERFACE flight_core_types)

# Composed helpers shared across variants (header-only): interface base
# classes stay pure, shared code lands here by composition.
add_library(platform_common INTERFACE)
target_include_directories(platform_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/common/include)
target_link_libraries(platform_common INTERFACE platform flight_core protocol)

if(DRONE_PLATFORM STREQUAL "stm32")
    add_subdirectory(src/stm32)
elseif(DRONE_PLATFORM STREQUAL "desktop")
    add_subdirectory(src/sim)
    add_subdirectory(src/replay)
else()
    message(FATAL_ERROR "unknown DRONE_PLATFORM '${DRONE_PLATFORM}'")
endif()
