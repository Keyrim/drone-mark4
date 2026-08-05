#pragma once

/// @file
/// @brief Body rate PI controller, the innermost control loop.

#include <array>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Tracks body rate setpoints with one PI loop per axis and produces the
    /// normalized torque demands the mixer distributes. Gains are normalized
    /// torque per rad/s of error; the integrator is clamped so a saturated
    /// output cannot wind it up.
    class RateController
    {
      public:
        /// Default roll and pitch proportional gain, sized for the simulated
        /// airframe: about 18 rad/s of closed loop bandwidth, safely under
        /// the motor lag pole.
        static constexpr float DEFAULT_KP_ROLL_PITCH = 0.03f;

        /// Default roll and pitch integral gain: slow bias absorption.
        static constexpr float DEFAULT_KI_ROLL_PITCH = 0.02f;

        /// Default yaw gains: the reaction torque authority is an order of
        /// magnitude weaker than the thrust differential, so yaw runs slower.
        static constexpr float DEFAULT_KP_YAW = 0.15f;
        static constexpr float DEFAULT_KI_YAW = 0.05f;

        /// Clamp on each integrator's contribution to the output.
        static constexpr float INTEGRAL_LIMIT = 0.2f;

        /// @brief Advances the three loops by one step.
        /// @param setpointRadS body rate setpoints [rad/s]
        /// @param gyroRadS measured body rates [rad/s]
        /// @param dt integration step [s]
        /// @return normalized torque demands about body x, y, z
        std::array<float, 3> update(const std::array<float, 3> &setpointRadS,
                                    const std::array<float, 3> &gyroRadS,
                                    float dt);

        /// @brief Clears the integrators (disarmed on the ground).
        void reset();

      private:
        std::array<float, 3> m_integral{}; ///< integrator state per axis
    };
} // namespace mark4
