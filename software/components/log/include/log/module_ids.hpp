#pragma once

/// @file
/// @brief Module ids of the code shared by more than one node, so the same
///        source file has the same id and name on every node. An
///        application's own modules take ids from LOG_MODULE_APP_BASE up,
///        in its log_modules.hpp.

#include <cstdint>

namespace mark4
{
    inline constexpr std::uint16_t LOG_MODULE_CORE = 1U;           ///< log/core
    inline constexpr std::uint16_t LOG_MODULE_TRANSPORT_UART = 2U; ///< transport/uart
    inline constexpr std::uint16_t LOG_MODULE_TRANSPORT_UDP = 3U;  ///< transport/udp
    inline constexpr std::uint16_t LOG_MODULE_PLATFORM_IMU = 16U;  ///< platform/imu
    inline constexpr std::uint16_t LOG_MODULE_PLATFORM_BARO = 17U; ///< platform/baro
    inline constexpr std::uint16_t LOG_MODULE_SIM_PLANT = 18U;     ///< sim/plant
    /// platform/telemetry
    inline constexpr std::uint16_t LOG_MODULE_PLATFORM_TELEMETRY = 19U;
    inline constexpr std::uint16_t LOG_MODULE_PLATFORM_ESC = 20U; ///< platform/esc
    inline constexpr std::uint16_t LOG_MODULE_OTA_STORE = 32U;    ///< ota/store
    inline constexpr std::uint16_t LOG_MODULE_OTA_UPDATER = 33U;  ///< ota/updater (the apps)

    /// First id an application picks for its own modules.
    inline constexpr std::uint16_t LOG_MODULE_APP_BASE = 256U;
} // namespace mark4
