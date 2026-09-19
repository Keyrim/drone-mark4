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
    /// telemetry/provider
    inline constexpr std::uint16_t LOG_MODULE_TELEMETRY_PROVIDER = 19U;
    /// status/provider
    inline constexpr std::uint16_t LOG_MODULE_STATUS_PROVIDER = 20U;
    inline constexpr std::uint16_t LOG_MODULE_LOG_PROVIDER = 21U;    ///< log/provider
    inline constexpr std::uint16_t LOG_MODULE_TUNING_PROVIDER = 22U; ///< tuning/provider
    /// status/consumer
    inline constexpr std::uint16_t LOG_MODULE_STATUS_CONSUMER = 23U;
    inline constexpr std::uint16_t LOG_MODULE_LOG_CONSUMER = 24U; ///< log/consumer
    /// telemetry/consumer
    inline constexpr std::uint16_t LOG_MODULE_TELEMETRY_CONSUMER = 25U;
    inline constexpr std::uint16_t LOG_MODULE_TUNING_CONSUMER = 26U; ///< tuning/consumer
    inline constexpr std::uint16_t LOG_MODULE_OTA_STORE = 32U;       ///< ota/store
    inline constexpr std::uint16_t LOG_MODULE_OTA_UPDATER = 33U;     ///< ota/updater (the apps)

    /// First id an application picks for its own modules.
    inline constexpr std::uint16_t LOG_MODULE_APP_BASE = 256U;
} // namespace mark4
