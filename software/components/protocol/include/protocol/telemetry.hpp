#pragma once

/// @file
/// @brief Telemetry packet, broadcast over UDP to any number of consumers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
#pragma pack(push, 1)
    /// Minimal state snapshot. Placeholder layout, not the final format.
    /// Frames follow the project convention: body x forward, y left, z up,
    /// right-handed; world z up; quaternions rotate body into world (w first).
    struct TelemetryPacket
    {
        std::uint8_t version;              ///< = PROTOCOL_VERSION
        std::uint8_t type;                 ///< = PacketType::TELEMETRY
        std::uint8_t sourceId;             ///< StreamSource of the sender
        std::uint16_t sequence;            ///< increments per packet sent, wraps
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
        std::uint8_t flightPhase;          ///< FlightPhase of the state machine
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + type (1) + source (1) + sequence (2)
    /// + timestamp (8) + gyro (12) + attitude quaternion (16) + gyro bias
    /// (12) + motors (16) + altitude (4) + vertical velocity (4) + throw
    /// state (1) + throw count (4) + release velocity (4) + apex timestamp
    /// (8) + apex altitude (4) + flight phase (1).
    inline constexpr std::size_t TELEMETRY_PACKET_SIZE = 99U;

    static_assert(sizeof(TelemetryPacket) == TELEMETRY_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<TelemetryPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(TelemetryPacket, version) == 0U);
    static_assert(offsetof(TelemetryPacket, type) == 1U);
    static_assert(offsetof(TelemetryPacket, sourceId) == 2U);
    static_assert(offsetof(TelemetryPacket, sequence) == 3U);
    static_assert(offsetof(TelemetryPacket, timestampUs) == 5U);
    static_assert(offsetof(TelemetryPacket, gyroRadS) == 13U);
    static_assert(offsetof(TelemetryPacket, attitudeQuat) == 25U);
    static_assert(offsetof(TelemetryPacket, gyroBiasRadS) == 41U);
    static_assert(offsetof(TelemetryPacket, motor) == 53U);
    static_assert(offsetof(TelemetryPacket, altitudeM) == 69U);
    static_assert(offsetof(TelemetryPacket, verticalVelocityMps) == 73U);
    static_assert(offsetof(TelemetryPacket, throwState) == 77U);
    static_assert(offsetof(TelemetryPacket, throwCount) == 78U);
    static_assert(offsetof(TelemetryPacket, releaseVelocityMps) == 82U);
    static_assert(offsetof(TelemetryPacket, apexTimestampUs) == 86U);
    static_assert(offsetof(TelemetryPacket, apexAltitudeM) == 94U);
    static_assert(offsetof(TelemetryPacket, flightPhase) == 98U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
