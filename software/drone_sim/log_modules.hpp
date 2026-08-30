#pragma once

/// @file
/// @brief Log module ids of drone_sim's own source files. The shared
///        libraries take theirs from log/module_ids.hpp.

#include <cstdint>

#include "log/module_ids.hpp"

namespace mark4
{
    inline constexpr std::uint16_t LOG_MODULE_APP_BOOT = LOG_MODULE_APP_BASE;      ///< app/boot
    inline constexpr std::uint16_t LOG_MODULE_APP_MAIN = LOG_MODULE_APP_BASE + 1U; ///< app/main
    inline constexpr std::uint16_t LOG_MODULE_SIM_LINK = LOG_MODULE_APP_BASE + 2U; ///< sim/link
    inline constexpr std::uint16_t LOG_MODULE_FLIGHT_CORE =
        LOG_MODULE_APP_BASE + 3U; ///< flight/core
} // namespace mark4
