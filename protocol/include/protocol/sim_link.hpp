#pragma once

/// @file
/// @brief Simulator link: sensor frames on the wire, simulator to flight core.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
#pragma pack(push, 1)
    /// Minimal sensor frame. Placeholder layout, not the final format
    /// (no lockstep handshake nor actuator packet yet).
    struct SimSensorPacket
    {
        std::uint8_t version;           ///< = PROTOCOL_VERSION
        std::uint64_t timestampUs;      ///< acquisition time [us]
        std::array<float, 3> gyroRadS;  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa;                   ///< static pressure [Pa]
    };
#pragma pack(pop)

    /// Packed wire size: version (1) + timestamp (8) + gyro (12) + accel (12) + baro (4).
    inline constexpr std::size_t SIM_SENSOR_PACKET_SIZE = 37U;

    static_assert(sizeof(SimSensorPacket) == SIM_SENSOR_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimSensorPacket>);
} // namespace mark4
