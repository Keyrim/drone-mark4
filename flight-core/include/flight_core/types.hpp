#pragma once

/// @file
/// @brief In-memory types exchanged with FlightCore (free to evolve,
///        distinct from the wire structs in protocol/).

#include <array>
#include <cstdint>

namespace mark4
{
    /// RC state. The kill switch is processed before anything else in step().
    struct RcInput
    {
        bool killSwitch = true; ///< defaults to safe: motors cut
        float throttle = 0.0f;  ///< normalized [0, 1]
    };

    /// Input of step(). Timestamped by platform at acquisition: the timestamp
    /// travels inside the frame, the flight core never reads a clock.
    struct SensorFrame
    {
        std::uint64_t timestampUs = 0U;   ///< acquisition time [us]
        std::array<float, 3> gyroRadS{};  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2{}; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa = 0.0f;              ///< static pressure [Pa]
        RcInput rc;                       ///< RC state
    };

    /// Output of step().
    struct ActuatorFrame
    {
        std::array<float, 4> motor{}; ///< normalized motor commands [0, 1]
    };
} // namespace mark4
