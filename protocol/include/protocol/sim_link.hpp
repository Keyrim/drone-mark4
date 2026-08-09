#pragma once

/// @file
/// @brief Simulator link: sensor frames in, actuator frames back to the sender.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
#pragma pack(push, 1)
    /// Sensor frame on the wire, simulator to flight process. Sensors only:
    /// the pilot state is not a sensor reading and does not travel here. It
    /// reaches the flight process out-of-band, as RcCommandPacket on its
    /// command receiver, exactly like it reaches the real board - so the
    /// fail-safe guarding a real flight is exercised in every simulated one.
    struct SimSensorPacket
    {
        std::uint8_t version;           ///< = PROTOCOL_VERSION
        std::uint8_t type;              ///< = PacketType::SIM_SENSOR
        std::uint64_t timestampUs;      ///< simulation time at acquisition [us]
        std::array<float, 3> gyroRadS;  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa;                   ///< static pressure [Pa]
        std::uint8_t resetCount;        ///< incremented on every simulator world
                                        ///< reset (teleport); wraps around. The
                                        ///< flight process discards its state on a
                                        ///< change: a teleport has no physical
                                        ///< counterpart an estimator could track.
                                        ///< A plant event, not an RC one: it stays.
    };

    /// Actuator frame on the wire, flight process back to the simulator. The
    /// echoed timestamp identifies the sensor packet this frame answers: it
    /// is the lockstep handshake, letting the simulator wait for the reply
    /// to the exact tick it sent (and resend on packet loss) instead of
    /// accepting whatever arrives.
    struct SimActuatorPacket
    {
        std::uint8_t version;          ///< = PROTOCOL_VERSION
        std::uint8_t type;             ///< = PacketType::SIM_ACTUATOR
        std::uint64_t echoTimestampUs; ///< timestamp of the sensor packet answered
        std::array<float, 4> motor;    ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// version (1) + type (1) + timestamp (8) + gyro (12) + accel (12)
    /// + baro (4) + reset count (1).
    inline constexpr std::size_t SIM_SENSOR_PACKET_SIZE = 39U;

    /// version (1) + type (1) + echoed timestamp (8) + motors (16).
    inline constexpr std::size_t SIM_ACTUATOR_PACKET_SIZE = 26U;

    static_assert(sizeof(SimSensorPacket) == SIM_SENSOR_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(SimActuatorPacket) == SIM_ACTUATOR_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimSensorPacket>);
    static_assert(std::is_trivially_copyable_v<SimActuatorPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(SimSensorPacket, version) == 0U);
    static_assert(offsetof(SimSensorPacket, type) == 1U);
    static_assert(offsetof(SimSensorPacket, timestampUs) == 2U);
    static_assert(offsetof(SimSensorPacket, gyroRadS) == 10U);
    static_assert(offsetof(SimSensorPacket, accelMps2) == 22U);
    static_assert(offsetof(SimSensorPacket, baroPa) == 34U);
    static_assert(offsetof(SimSensorPacket, resetCount) == 38U);
    static_assert(offsetof(SimActuatorPacket, version) == 0U);
    static_assert(offsetof(SimActuatorPacket, type) == 1U);
    static_assert(offsetof(SimActuatorPacket, echoTimestampUs) == 2U);
    static_assert(offsetof(SimActuatorPacket, motor) == 10U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
