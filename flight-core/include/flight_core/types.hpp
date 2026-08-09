#pragma once

/// @file
/// @brief In-memory types exchanged with FlightCore (free to evolve,
///        distinct from the wire structs in protocol/).

#include <array>
#include <cstdint>

namespace mark4
{
    /// Standard gravity [m/s^2], the reference for accelerometer readings.
    inline constexpr float GRAVITY_MPS2 = 9.80665f;

    /// Unit quaternion representing a body-to-world rotation. Identity by
    /// default. Kept a plain aggregate: the math lives with its users.
    struct Quaternion
    {
        float w = 1.0f; ///< scalar part
        float x = 0.0f; ///< vector part, body x
        float y = 0.0f; ///< vector part, body y
        float z = 0.0f; ///< vector part, body z
    };

    /// Piloting mode selected by the pilot. MANUAL is 0 so a zeroed RcInput
    /// (the fail-safe state) lands on the mode that never flies on its own.
    /// The values mirror the wire-level RC_MODE_* constants one for one; the
    /// platform adapter that decodes the uplink is where the two meet and
    /// where they are asserted equal, so flight-core stays wire-free.
    enum class PilotMode : std::uint8_t
    {
        MANUAL = 0U,        ///< the stick commands the collective directly
        ALTITUDE_AUTO = 1U, ///< the stick commands a vertical velocity
    };

    /// RC state. The kill switch is processed before anything else in step().
    struct RcInput
    {
        bool killSwitch = true;             ///< defaults to safe: motors cut
        float throttle = 0.0f;              ///< normalized [0, 1]
        bool armSwitch = false;             ///< true = motors may run
        PilotMode mode = PilotMode::MANUAL; ///< piloting mode, read while disarmed
    };

    /// Input of step(). Timestamped by platform at acquisition: the timestamp
    /// travels inside the frame, the flight core never reads a clock.
    /// Body frame convention, for every producer: x forward, y left, z up,
    /// right-handed - the accelerometer reads +GRAVITY_MPS2 on z at rest.
    struct SensorFrame
    {
        std::uint64_t timestampUs = 0U;   ///< acquisition time [us]
        std::array<float, 3> gyroRadS{};  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2{}; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa = 0.0f;              ///< static pressure [Pa]
        RcInput rc;                       ///< RC state
    };

    /// Output of step(). Stamped with the timestamp of the sensor frame it
    /// answers, so a consumer can pair outputs with inputs (the sim link
    /// lockstep relies on it).
    struct ActuatorFrame
    {
        std::uint64_t timestampUs = 0U; ///< timestamp of the answered SensorFrame [us]
        std::array<float, 4> motor{};   ///< normalized motor commands [0, 1]
    };
} // namespace mark4
