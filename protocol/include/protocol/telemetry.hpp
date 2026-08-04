#pragma once

/// @file
/// @brief Telemetry packet, broadcast over UDP to any number of consumers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
#pragma pack(push, 1)
    /// Minimal state snapshot. Placeholder layout, not the final format.
    struct TelemetryPacket
    {
        std::uint8_t version;          ///< = PROTOCOL_VERSION
        std::uint64_t timestampUs;     ///< acquisition time [us]
        std::array<float, 3> gyroRadS; ///< body angular rates [rad/s]
        std::array<float, 4> motor;    ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + timestamp (8) + gyro (12) + motors (16).
    inline constexpr std::size_t TELEMETRY_PACKET_SIZE = 37U;

    static_assert(sizeof(TelemetryPacket) == TELEMETRY_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<TelemetryPacket>);
} // namespace mark4
