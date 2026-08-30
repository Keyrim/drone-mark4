#pragma once

/// @file
/// @brief Log module ids of the firmware's own source files. The shared
///        libraries take theirs from log/module_ids.hpp.

#include <cstdint>

#include "log/module_ids.hpp"

namespace mark4
{
    inline constexpr std::uint16_t LOG_MODULE_APP_BOOT = LOG_MODULE_APP_BASE;        ///< app/boot
    inline constexpr std::uint16_t LOG_MODULE_APP_STATUS = LOG_MODULE_APP_BASE + 1U; ///< app/status
    inline constexpr std::uint16_t LOG_MODULE_RC = LOG_MODULE_APP_BASE + 2U;         ///< rc
    inline constexpr std::uint16_t LOG_MODULE_FLIGHT_CORE =
        LOG_MODULE_APP_BASE + 3U; ///< flight/core
} // namespace mark4
