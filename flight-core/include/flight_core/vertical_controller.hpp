#pragma once

/// @file
/// @brief Vertical velocity to collective command controller.

#include "flight_core/types.hpp"

namespace mark4
{
    /// Tracks a vertical velocity setpoint with a PI loop around a hover
    /// feedforward, and produces the collective motor command the mixer
    /// spreads over the four motors.
    class VerticalController
    {
      public:
        /// Default collective producing about one weight of thrust on the
        /// simulated airframe. A calibration knob by nature: the real
        /// airframe will have its own value, and the integrator absorbs
        /// what the guess misses.
        static constexpr float DEFAULT_HOVER_COLLECTIVE = 0.52f;

        /// Default proportional gain [collective per m/s]: about 4 rad/s of
        /// velocity loop bandwidth on the simulated airframe.
        static constexpr float DEFAULT_KP = 0.1f;

        /// Default integral gain [collective per m].
        static constexpr float DEFAULT_KI = 0.05f;

        /// Clamp on the integrator's contribution to the output.
        static constexpr float INTEGRAL_LIMIT = 0.2f;

        /// @param hoverCollective feedforward collective at hover [0, 1]
        explicit VerticalController(float hoverCollective = DEFAULT_HOVER_COLLECTIVE)
            : m_hoverCollective(hoverCollective)
        {
        }

        /// @brief Advances the loop by one step.
        /// @param setpointMps vertical velocity setpoint, positive up [m/s]
        /// @param verticalVelocityMps estimated vertical velocity [m/s]
        /// @param dt integration step [s]
        /// @return collective command [0, 1]
        float update(float setpointMps, float verticalVelocityMps, float dt);

        /// @brief Clears the integrator (disarmed on the ground).
        void reset();

      private:
        float m_hoverCollective; ///< feedforward collective at hover
        float m_integral = 0.0f; ///< integrator state
    };
} // namespace mark4
