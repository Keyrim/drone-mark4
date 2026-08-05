#pragma once

/// @file
/// @brief Attitude to body rate controller (reduced attitude, tilt only).

#include <array>

#include "flight_core/types.hpp"

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

        /// @brief Computes the body rate setpoints leveling the drone.
        /// @param attitude current body-to-world attitude estimate
        /// @return rate setpoints [rad/s], zero yaw
        [[nodiscard]] std::array<float, 3> rateSetpointRadS(const Quaternion &attitude) const;

      private:
        float m_kp; ///< proportional gain [1/s]
    };
} // namespace mark4
