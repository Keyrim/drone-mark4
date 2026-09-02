#pragma once

/// @file
/// @brief Attitude to body rate controller (reduced attitude, tilt only).

#include <array>

#include "flight_core/types.hpp"
#include "telemetry/registry.hpp"

namespace mark4
{
    /// Turns the attitude error into body rate setpoints. Only the tilt is
    /// controlled - the rotation bringing the body up axis onto the world up
    /// axis - because a hover does not care about heading: yaw is left to the
    /// rate loop with a zero setpoint, which damps it without steering it.
    /// This is also exactly the recovery primitive an arbitrary-attitude
    /// throw needs.
    class AttitudeController
    {
      public:
        /// Default proportional gain [1/s]: the outer loop runs at about
        /// 5 rad/s, well under the rate loop bandwidth.
        static constexpr float DEFAULT_KP = 5.0f;

        /// @param kp proportional gain on the tilt error [1/s]
        explicit AttitudeController(float kp = DEFAULT_KP)
            : m_kp(kp)
        {
        }

        /// @brief Sets the proportional gain. Callable between two steps; this
        ///        loop is stateless, so the change takes effect on the next
        ///        setpoint and nothing else.
        /// @param kp proportional gain on the tilt error [1/s]
        void setKp(float kp)
        {
            m_kp = kp;
        }

        /// @brief Computes the body rate setpoints tilting the drone so its
        ///        thrust axis reaches the desired world direction. The default
        ///        direction is straight up: a pure leveling. A braking or
        ///        translating flight tilts the target instead.
        ///        Not const: the loop records the tilt error and the
        ///        setpoint it produced, which is what the telemetry registry
        ///        exposes of it.
        /// @param attitude current body-to-world attitude estimate
        /// @param desiredUpWorld unit direction the body up axis should reach
        /// @return rate setpoints [rad/s], zero yaw
        [[nodiscard]] std::array<float, 3> rateSetpointRadS(
            const Quaternion &attitude,
            const std::array<float, 3> &desiredUpWorld = {0.0f, 0.0f, 1.0f});

      private:
        float m_kp; ///< proportional gain [1/s]

        // What the last call computed, for the telemetry registry. The error
        // is the sine of the angle between the body up axis and the desired
        // one, per axis; this loop is a pure P, so the output is the error
        // times the gain and there is no integral or derivative to expose.
        std::array<float, 3> m_errorSin{};     ///< tilt error, sin of the angle
        std::array<float, 3> m_setpointRadS{}; ///< rate setpoint produced [rad/s]

        TelemetryEntry m_errorRoll{"attitude/roll/error", TelemetryUnit::UNITLESS, m_errorSin[0]};
        TelemetryEntry m_errorPitch{"attitude/pitch/error", TelemetryUnit::UNITLESS, m_errorSin[1]};
        TelemetryEntry m_outputRoll{
            "attitude/roll/output", TelemetryUnit::RAD_PER_S, m_setpointRadS[0]};
        TelemetryEntry m_outputPitch{
            "attitude/pitch/output", TelemetryUnit::RAD_PER_S, m_setpointRadS[1]};
    };
} // namespace mark4
