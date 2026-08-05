#pragma once

/// @file
/// @brief Telemetry packet, broadcast over UDP to any number of consumers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
    /// UDP port telemetry is broadcast to; any number of consumers may listen.
    inline constexpr std::uint16_t TELEMETRY_PORT = 47801U;

#pragma pack(push, 1)
    /// Minimal state snapshot. Placeholder layout, not the final format.
    /// Frames follow the project convention: body x forward, y left, z up,
    /// right-handed; world z up; quaternions rotate body into world (w first).
    struct TelemetryPacket
    {
        std::uint8_t version;              ///< = PROTOCOL_VERSION
        std::uint64_t timestampUs;         ///< acquisition time [us]
        std::array<float, 3> gyroRadS;     ///< body angular rates [rad/s]
        std::array<float, 4> attitudeQuat; ///< estimated attitude, w x y z
        std::array<float, 3> gyroBiasRadS; ///< estimated gyro bias [rad/s]
        std::array<float, 4> motor;        ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + timestamp (8) + gyro (12) + attitude
    /// quaternion (16) + gyro bias (12) + motors (16).
    inline constexpr std::size_t TELEMETRY_PACKET_SIZE = 65U;

    static_assert(sizeof(TelemetryPacket) == TELEMETRY_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<TelemetryPacket>);
} // namespace mark4
