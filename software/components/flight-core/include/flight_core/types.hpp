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
    /// (the fail-safe state) lands on the mode that flies the least on its
    /// own: no leveling, no altitude loop, the sticks alone. The values
    /// mirror the wire-level RC_MODE_* constants one for one; the platform
    /// adapter that decodes the uplink is where the two meet and where they
    /// are asserted equal, so flight-core stays wire-free.
    enum class PilotMode : std::uint8_t
    {
        MANUAL = 0U,        ///< sticks command body rates, the throttle is the collective
        ALTITUDE_AUTO = 1U, ///< sticks command the tilt, the throttle a vertical velocity
        LEVEL = 2U,         ///< sticks command the tilt, the throttle is the collective
    };

    /// RC state. The kill switch is processed before anything else in step().
    /// The three sticks are the pilot's, not the body's: positive means
    /// right, forward and clockwise as seen by someone holding the
    /// transmitter, and the core maps them onto the body frame (x forward,
    /// y left, z up) where it needs them. Deadband and scaling are the
    /// core's business too: the values here are the raw stick positions.
    struct RcInput
    {
        bool killSwitch = true;             ///< defaults to safe: motors cut
        float throttle = 0.0f;              ///< normalized [0, 1]
        float roll = 0.0f;                  ///< normalized [-1, 1], positive rolls right
        float pitch = 0.0f;                 ///< normalized [-1, 1], positive noses down
        float yaw = 0.0f;                   ///< normalized [-1, 1], positive turns right
        bool armSwitch = false;             ///< true = motors may run
        PilotMode mode = PilotMode::MANUAL; ///< piloting mode, read while disarmed
    };

    /// Input of step(). Timestamped by platform at acquisition: the timestamp
    /// travels inside the frame, the flight core never reads a clock.
    /// Body frame convention, for every producer: x forward, y left, z up,
    /// right-handed - the accelerometer reads +GRAVITY_MPS2 on z at rest.
    ///
    /// Validity contract: a sensor field is valid only when it is a fresh
    /// measurement acquired for this frame. A platform that could not read
    /// the sensor leaves the flag false and the field zero; it never replays
    /// an old sample. The flags default to false so a frame nobody filled
    /// is a frame without sensors.
    struct SensorFrame
    {
        std::uint64_t timestampUs = 0U;   ///< acquisition time [us]
        bool imuValid = false;            ///< gyroRadS and accelMps2 are fresh measurements
        bool baroValid = false;           ///< baroPa is a fresh measurement
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
