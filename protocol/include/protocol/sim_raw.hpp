#pragma once

/// @file
/// @brief Raw simulator state, broadcast over UDP for estimator validation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
#pragma pack(push, 1)
    /// Exact simulator state, straight from the physics engine: no sensor
    /// model, no noise, no estimation. Only a simulator can produce it.
    /// Frames follow the project convention: body x forward, y left, z up,
    /// right-handed; world z up; quaternions rotate body into world (w first).
    struct SimRawPacket
    {
        std::uint8_t version;              ///< = PROTOCOL_VERSION
        std::uint64_t timestampUs;         ///< simulated time [us]
        std::array<float, 4> attitudeQuat; ///< exact attitude, w x y z
        std::array<float, 3> positionM;    ///< world position [m]
        std::array<float, 3> velocityMps;  ///< world linear velocity [m/s]
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + timestamp (8) + attitude quaternion
    /// (16) + position (12) + velocity (12).
    inline constexpr std::size_t SIM_RAW_PACKET_SIZE = 49U;

    static_assert(sizeof(SimRawPacket) == SIM_RAW_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimRawPacket>);
} // namespace mark4
