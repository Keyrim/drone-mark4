#pragma once

/// @file
/// @brief Log module ids of the relay's own source files. The shared
///        libraries take theirs from log/module_ids.hpp.

#include <cstdint>

#include "log/module_ids.hpp"

namespace mark4
{
    inline constexpr std::uint16_t LOG_MODULE_APP_BOOT = LOG_MODULE_APP_BASE;        ///< app/boot
    inline constexpr std::uint16_t LOG_MODULE_APP_WIFI = LOG_MODULE_APP_BASE + 1U;   ///< app/wifi
    inline constexpr std::uint16_t LOG_MODULE_RELAY_CORE = LOG_MODULE_APP_BASE + 2U; ///< relay/core
    inline constexpr std::uint16_t LOG_MODULE_RELAY_STATS =
        LOG_MODULE_APP_BASE + 3U;                                                   ///< relay/stats
    inline constexpr std::uint16_t LOG_MODULE_RELAY_OTA = LOG_MODULE_APP_BASE + 4U; ///< relay/ota
} // namespace mark4
