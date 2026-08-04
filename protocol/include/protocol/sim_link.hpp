#pragma once

/// @file
/// @brief Simulator link: sensor frames in, actuator frames back to the sender.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
    /// UDP port the flight process listens on for sensor packets. Actuator
    /// packets are sent back to the address and port the last sensor packet
    /// came from, so the simulator needs no listening port of its own.
    inline constexpr std::uint16_t SIM_LINK_PORT = 47800U;

#pragma pack(push, 1)
    /// Sensor frame on the wire, simulator to flight process. Placeholder
    /// layout, not the final format (no lockstep handshake yet).
    struct SimSensorPacket
    {
        std::uint8_t version;           ///< = PROTOCOL_VERSION
        std::uint64_t timestampUs;      ///< simulation time at acquisition [us]
        std::array<float, 3> gyroRadS;  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa;                   ///< static pressure [Pa]
        std::uint8_t killSwitch;        ///< 1 = engaged (motors cut), 0 = released
        float throttle;                 ///< normalized RC throttle [0, 1]
    };

    /// Actuator frame on the wire, flight process back to the simulator.
    struct SimActuatorPacket
    {
        std::uint8_t version;       ///< = PROTOCOL_VERSION
        std::array<float, 4> motor; ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// version (1) + timestamp (8) + gyro (12) + accel (12) + baro (4)
    /// + kill switch (1) + throttle (4).
    inline constexpr std::size_t SIM_SENSOR_PACKET_SIZE = 42U;

    /// version (1) + motors (16).
    inline constexpr std::size_t SIM_ACTUATOR_PACKET_SIZE = 17U;

    static_assert(sizeof(SimSensorPacket) == SIM_SENSOR_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(SimActuatorPacket) == SIM_ACTUATOR_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimSensorPacket>);
    static_assert(std::is_trivially_copyable_v<SimActuatorPacket>);
} // namespace mark4
