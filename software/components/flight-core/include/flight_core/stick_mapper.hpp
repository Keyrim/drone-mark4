#pragma once

/// @file
/// @brief Stick positions to control setpoints: the pilot's intent, written
///        in the pilot's terms, turned into what the loops track.

#include <array>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Turns the three sticks of an RcInput into the setpoints the piloted
    /// modes track: body rates for the direct rate mode, a desired thrust
    /// direction plus a yaw rate for the leveling one. Stateless: the
    /// deadband and the ranges below are the feel of the drone, tunable
    /// like a gain, and the only thing this class holds.
    ///
    /// The sticks arrive in the pilot's convention (positive = right,
    /// forward, clockwise) and leave in the body frame (x forward, y left,
    /// z up, right-handed): a right roll is a positive rate about x, a nose
    /// down pitch a positive rate about y, a clockwise yaw a negative rate
    /// about z. This is the one place the two conventions meet.
    class StickMapper
    {
      public:
        /// Body rate at full roll or pitch deflection [rad/s], about
        /// 200 deg/s: brisk enough to feel the loop, far from the gyro's
        /// full scale and from the rate cutoff.
        static constexpr float DEFAULT_RATE_ROLL_PITCH_RADS = 3.5f;

        /// Body rate at full yaw deflection [rad/s], about 150 deg/s: the
        /// yaw authority of a quad is weaker, asking for more only winds the
        /// integrator up.
        static constexpr float DEFAULT_RATE_YAW_RADS = 2.6f;

        /// Tilt at full roll or pitch deflection in the leveling mode [rad],
        /// about 30 deg: a comfortable lean that keeps most of the thrust
        /// pointing up.
        static constexpr float DEFAULT_TILT_MAX_RAD = 0.5f;

        /// Half-width of the band around the centre read as no deflection.
        /// A spring-loaded stick never returns to the exact same point;
        /// inside this band the setpoint is exactly zero, so a released
        /// stick asks for nothing.
        static constexpr float DEFAULT_DEADBAND = 0.05f;

        /// @param rateRollPitchRadS body rate at full roll or pitch deflection [rad/s]
        /// @param rateYawRadS body rate at full yaw deflection [rad/s]
        /// @param tiltMaxRad tilt at full deflection in the leveling mode [rad]
        /// @param deadband half-width of the centre band read as zero
        explicit StickMapper(float rateRollPitchRadS = DEFAULT_RATE_ROLL_PITCH_RADS,
                             float rateYawRadS = DEFAULT_RATE_YAW_RADS,
                             float tiltMaxRad = DEFAULT_TILT_MAX_RAD,
                             float deadband = DEFAULT_DEADBAND);

        /// @brief Sets the body rate at full roll or pitch deflection.
        ///        Callable between two steps: the mapping is stateless, the
        ///        next setpoint uses it.
        /// @param rateRadS body rate at full deflection [rad/s]
        void setRateRollPitch(float rateRadS)
        {
            m_rateRollPitchRadS = rateRadS;
        }

        /// @brief Sets the body rate at full yaw deflection. Callable between
        ///        two steps.
        /// @param rateRadS body rate at full deflection [rad/s]
        void setRateYaw(float rateRadS)
        {
            m_rateYawRadS = rateRadS;
        }

        /// @brief Sets the tilt at full deflection in the leveling mode.
        ///        Callable between two steps.
        /// @param tiltRad tilt at full deflection [rad], under a right angle
        void setTiltMax(float tiltRad);

        /// @brief Sets the centre deadband. Callable between two steps.
        /// @param deadband half-width of the centre band, in [0, 1)
        void setDeadband(float deadband)
        {
            m_deadband = deadband;
        }

        /// @brief Maps one stick to its deflection past the deadband: zero
        ///        inside the band, then a linear ramp reaching exactly 1 at
        ///        full stick, so the map is continuous at the band's edge.
        ///        Clamped: a stick reporting beyond its range is full stick.
        /// @param stick raw stick position, nominally in [-1, 1]
        /// @return deflection in [-1, 1]
        [[nodiscard]] float deflection(float stick) const;

        /// @brief Body rate setpoints of the direct rate mode: each stick
        ///        commands the rate about its axis, and the released sticks
        ///        command zero on every axis.
        /// @param rc RC state of the frame
        /// @return body rate setpoints [rad/s]
        [[nodiscard]] std::array<float, 3> rateSetpointRadS(const RcInput &rc) const;

        /// @brief Direction the body up axis should reach in the leveling
        ///        mode, as a world unit vector: straight up with the sticks
        ///        released, leaned toward the pilot's roll and pitch
        ///        otherwise, the lean measured in the drone's own heading
        ///        (forward is where the nose points, not where it pointed at
        ///        takeoff) and bounded by the maximum tilt.
        /// @param rc RC state of the frame
        /// @param attitude current body-to-world attitude estimate, source of
        ///        the heading
        /// @return unit direction in the world frame
        [[nodiscard]] std::array<float, 3> desiredUpWorld(const RcInput &rc,
                                                          const Quaternion &attitude) const;

        /// @brief Yaw rate setpoint of the leveling mode, where the attitude
        ///        loop leaves yaw alone: the yaw stick steers the heading at
        ///        a rate, it never holds one.
        /// @param rc RC state of the frame
        /// @return body rate setpoint about z [rad/s]
        [[nodiscard]] float yawRateRadS(const RcInput &rc) const;

        /// @return body rate at full roll or pitch deflection [rad/s]
        [[nodiscard]] float rateRollPitchRadS() const
        {
            return m_rateRollPitchRadS;
        }

        /// @return body rate at full yaw deflection [rad/s]
        [[nodiscard]] float rateYawRadS() const
        {
            return m_rateYawRadS;
        }

        /// @return tilt at full deflection in the leveling mode [rad]
        [[nodiscard]] float tiltMaxRad() const
        {
            return m_tiltMaxRad;
        }

        /// @return half-width of the centre deadband
        [[nodiscard]] float deadband() const
        {
            return m_deadband;
        }

      private:
        float m_rateRollPitchRadS; ///< body rate at full roll or pitch deflection [rad/s]
        float m_rateYawRadS;       ///< body rate at full yaw deflection [rad/s]
        float m_tiltMaxRad;        ///< tilt at full deflection in the leveling mode [rad]
        float m_tanTiltMax;        ///< its tangent, the lean at full deflection
        float m_deadband;          ///< half-width of the centre band read as zero
    };
} // namespace mark4
