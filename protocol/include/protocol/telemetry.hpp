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
        float altitudeM;                   ///< estimated altitude above startup [m]
        float verticalVelocityMps;         ///< estimated vertical velocity, up [m/s]
        std::uint8_t throwState;           ///< ThrowState of the detector
        std::uint32_t throwCount;          ///< throws detected since startup
        float releaseVelocityMps;          ///< last release velocity [m/s]
        std::uint64_t apexTimestampUs;     ///< last predicted apex instant [us]
        float apexAltitudeM;               ///< last predicted apex altitude [m]
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + timestamp (8) + gyro (12) + attitude
    /// quaternion (16) + gyro bias (12) + motors (16) + altitude (4) +
    /// vertical velocity (4) + throw state (1) + throw count (4) + release
    /// velocity (4) + apex timestamp (8) + apex altitude (4).
    inline constexpr std::size_t TELEMETRY_PACKET_SIZE = 94U;

    static_assert(sizeof(TelemetryPacket) == TELEMETRY_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<TelemetryPacket>);
} // namespace mark4
