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

        /// @param kpRollPitch proportional gain on roll and pitch
        /// @param kiRollPitch integral gain on roll and pitch
        /// @param kpYaw proportional gain on yaw
        /// @param kiYaw integral gain on yaw
        explicit RateController(float kpRollPitch = DEFAULT_KP_ROLL_PITCH,
                                float kiRollPitch = DEFAULT_KI_ROLL_PITCH,
                                float kpYaw = DEFAULT_KP_YAW,
                                float kiYaw = DEFAULT_KI_YAW)
            : m_kp{kpRollPitch, kpRollPitch, kpYaw},
              m_ki{kiRollPitch, kiRollPitch, kiYaw}
        {
        }

        /// @brief Sets the roll and pitch proportional gain. Callable between
        ///        two steps; the integrator state is deliberately preserved so
        ///        a retune does not kick the loop.
        /// @param kp proportional gain on roll and pitch
        void setKpRollPitch(float kp)
        {
            m_kp[0] = kp;
            m_kp[1] = kp;
        }

        /// @brief Sets the roll and pitch integral gain. Callable between two
        ///        steps; the integrator state is deliberately preserved so a
        ///        retune does not kick the loop.
        /// @param ki integral gain on roll and pitch
        void setKiRollPitch(float ki)
        {
            m_ki[0] = ki;
            m_ki[1] = ki;
        }

        /// @brief Sets the yaw proportional gain. Callable between two steps;
        ///        the integrator state is deliberately preserved so a retune
        ///        does not kick the loop.
        /// @param kp proportional gain on yaw
        void setKpYaw(float kp)
        {
            m_kp[2] = kp;
        }

        /// @brief Sets the yaw integral gain. Callable between two steps; the
        ///        integrator state is deliberately preserved so a retune does
        ///        not kick the loop.
        /// @param ki integral gain on yaw
        void setKiYaw(float ki)
        {
            m_ki[2] = ki;
        }

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
        std::array<float, 3> m_kp;         ///< proportional gain per axis
        std::array<float, 3> m_ki;         ///< integral gain per axis
        std::array<float, 3> m_integral{}; ///< integrator state per axis
    };
} // namespace mark4
