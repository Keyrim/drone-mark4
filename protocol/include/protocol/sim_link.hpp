#pragma once

/// @file
/// @brief Simulator link: sensor frames in, actuator frames back to the sender.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
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
        std::uint8_t armSwitch;         ///< 1 = armed for an autonomous throw flight
        std::uint8_t resetCount;        ///< incremented on every simulator world
                                        ///< reset (teleport); wraps around. The
                                        ///< flight process discards its state on a
                                        ///< change: a teleport has no physical
                                        ///< counterpart an estimator could track.
    };

    /// Actuator frame on the wire, flight process back to the simulator. The
    /// echoed timestamp identifies the sensor packet this frame answers: it
    /// is the lockstep handshake, letting the simulator wait for the reply
    /// to the exact tick it sent (and resend on packet loss) instead of
    /// accepting whatever arrives.
    struct SimActuatorPacket
    {
        std::uint8_t version;          ///< = PROTOCOL_VERSION
        std::uint64_t echoTimestampUs; ///< timestamp of the sensor packet answered
        std::array<float, 4> motor;    ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// version (1) + timestamp (8) + gyro (12) + accel (12) + baro (4)
    /// + kill switch (1) + throttle (4) + arm switch (1) + reset count (1).
    inline constexpr std::size_t SIM_SENSOR_PACKET_SIZE = 44U;

    /// version (1) + echoed timestamp (8) + motors (16).
    inline constexpr std::size_t SIM_ACTUATOR_PACKET_SIZE = 25U;

    static_assert(sizeof(SimSensorPacket) == SIM_SENSOR_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(SimActuatorPacket) == SIM_ACTUATOR_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimSensorPacket>);
    static_assert(std::is_trivially_copyable_v<SimActuatorPacket>);
} // namespace mark4
