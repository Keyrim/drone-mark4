#pragma once

/// @file
/// @brief Log module ids of the hub's own source files. The shared
///        libraries take theirs from log/module_ids.hpp.

#include <cstdint>

#include "log/module_ids.hpp"

namespace mark4
{
    inline constexpr std::uint16_t LOG_MODULE_APP_MAIN = LOG_MODULE_APP_BASE; ///< app/main
    inline constexpr std::uint16_t LOG_MODULE_GATEWAY_CORE =
        LOG_MODULE_APP_BASE + 1U; ///< gateway/core
    inline constexpr std::uint16_t LOG_MODULE_GATEWAY_WS = LOG_MODULE_APP_BASE + 2U; ///< gateway/ws
} // namespace mark4
