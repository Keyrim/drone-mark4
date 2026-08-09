#pragma once

/// @file
/// @brief Vertical state estimator (complementary filter, baro + accel).

#include <array>
#include <cstdint>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Estimates the altitude and vertical velocity by fusing the barometric
    /// altitude (accurate long term, noisy short term) with the integrated
    /// vertical acceleration (smooth and immediate, drifts). The specific
    /// force is rotated into the world frame with the estimated attitude, so
    /// free fall needs no special case: the accelerometer reads 0 g and the
    /// integration alone produces the -g slope of the velocity.
    ///
    /// The altitude is relative to a baro reference averaged over the first
    /// REFERENCE_SAMPLES resting frames: the estimator reads zero where it
    /// woke up. Only quasi-static frames (plausible baro, accel norm near
    /// 1 g, quiet gyro) may seed the reference: a core booting mid-motion -
    /// the in-air reboot case above all - would otherwise average a
    /// worthless reference out of a changing pressure. Rebooted in flight,
    /// the capture simply completes once the drone is back at rest, and
    /// ready() stays false until then.
    ///
    /// The baro channel is gated, because an MS5611 over I2C will produce
    /// garbage eventually: a pressure outside the plausible window is a
    /// sensor fault and contributes nothing (the estimate coasts on the
    /// accelerometer), and the innovation of a plausible sample is clamped,
    /// so one glitched frame moves the estimate by millimeters instead of
    /// railing the vertical loop.
    ///
    /// The horizontal velocity is dead reckoned from the same world frame
    /// projection: nothing measures it (no GPS, no flow), so the integration
    /// leaks toward zero with HORIZONTAL_LEAK_S. Anchored by the rest before
    /// a throw, it is accurate for the few seconds where braking the throw's
    /// momentum matters, and harmlessly fades afterwards.
    class VerticalEstimator
    {
      public:
        /// Default altitude correction gain [1/s]: with DEFAULT_VELOCITY_GAIN
        /// it forms a ~2 rad/s, well damped baro tracking loop - slow enough
        /// to average the baro noise, fast enough for a throw.
        static constexpr float DEFAULT_ALTITUDE_GAIN = 2.8f;

        /// Default velocity correction gain [1/s^2].
        static constexpr float DEFAULT_VELOCITY_GAIN = 4.0f;

        /// Number of frames averaged into the baro reference at startup.
        static constexpr std::uint32_t REFERENCE_SAMPLES = 50U;

        /// Half-width of the accel norm window around 1 g inside which a
        /// frame may count as resting for the reference capture: a falling
        /// or thrusting drone reads far from 1 g and must not seed it.
        static constexpr float REFERENCE_ACCEL_GATE_MPS2 = 1.0f;

        /// Gyro norm under which a frame may count as resting for the
        /// reference capture: a tumbling drone is not on the ground.
        static constexpr float REFERENCE_GYRO_QUIET_RADS = 1.5f;

        /// Lower bound of the plausible static pressure window [Pa], about
        /// 9000 m: a hand-thrown drone has no business past it, a zeroed or
        /// glitched frame lands far below. Outside the window the sample is
        /// a fault, not a measurement: it neither seeds the reference nor
        /// corrects the estimate.
        static constexpr float MIN_PLAUSIBLE_PA = 30000.0f;

        /// Upper bound of the plausible static pressure window [Pa], with
        /// margin below sea level.
        static constexpr float MAX_PLAUSIBLE_PA = 110000.0f;

        /// Clamp on the baro innovation [m]: a plausible but glitched sample
        /// pulls the estimate by at most gain * MAX_INNOVATION_M * dt per
        /// frame (millimeters), while a real standing error still converges
        /// at a brisk, bounded rate.
        static constexpr float MAX_INNOVATION_M = 2.0f;

        /// Leak time constant of the dead reckoned horizontal velocity [s]:
        /// long against a throw plus its braking, short against the drift of
        /// an unaided integration.
        static constexpr float HORIZONTAL_LEAK_S = 8.0f;

        /// @param altitudeGain correction gain on the altitude [1/s]
        /// @param velocityGain correction gain on the velocity [1/s^2]
        explicit VerticalEstimator(float altitudeGain = DEFAULT_ALTITUDE_GAIN,
                                   float velocityGain = DEFAULT_VELOCITY_GAIN)
            : m_altitudeGain(altitudeGain),
              m_velocityGain(velocityGain)
        {
        }

        /// @brief Sets the altitude correction gain. Callable between two
        ///        steps; the altitude and velocity estimates are left as they
        ///        are, the new gain only shapes how the next frames correct them.
        /// @param altitudeGain correction gain on the altitude [1/s]
        void setAltitudeGain(float altitudeGain)
        {
            m_altitudeGain = altitudeGain;
        }

        /// @brief Sets the velocity correction gain. Callable between two
        ///        steps; the altitude and velocity estimates are left as they
        ///        are, the new gain only shapes how the next frames correct them.
        /// @param velocityGain correction gain on the velocity [1/s^2]
        void setVelocityGain(float velocityGain)
        {
            m_velocityGain = velocityGain;
        }

        /// @brief Advances the estimate with one sensor frame. The first
        ///        REFERENCE_SAMPLES resting frames only build the baro reference. The
        ///        caller owns the time policy (monotonicity, gap handling) and
        ///        hands the integration step down; a non-positive dt only
        ///        contributes to the reference, nothing integrates.
        /// @param frame sensor frame carrying accel and baro
        /// @param dtS integration step [s], 0 when nothing may integrate
        ///        (first frame or gap in the stream)
        /// @param attitude current body-to-world attitude estimate
        void update(const SensorFrame &frame, float dtS, const Quaternion &attitude);

        /// @return true once the baro reference is captured and the estimate runs
        [[nodiscard]] bool ready() const
        {
            return m_ready;
        }

        /// @return altitude above the startup reference [m]
        [[nodiscard]] float altitudeM() const
        {
            return m_altitudeM;
        }

        /// @return vertical velocity, positive up [m/s]
        [[nodiscard]] float verticalVelocityMps() const
        {
            return m_velocityMps;
        }

        /// @return dead reckoned world horizontal velocity, x and y [m/s]
        [[nodiscard]] const std::array<float, 2> &horizontalVelocityMps() const
        {
            return m_horizontalMps;
        }

        /// @brief Standard atmosphere altitude for a static pressure.
        /// @param pressurePa static pressure [Pa], clamped to a sane range
        /// @return absolute altitude above the standard sea level [m]
        static float PressureAltitudeM(float pressurePa);

      private:
        float m_altitudeGain;                   ///< altitude correction gain [1/s]
        float m_velocityGain;                   ///< velocity correction gain [1/s^2]
        float m_altitudeM = 0.0f;               ///< estimate, relative to the reference [m]
        float m_velocityMps = 0.0f;             ///< estimate, positive up [m/s]
        std::array<float, 2> m_horizontalMps{}; ///< dead reckoned world velocity [m/s]
        float m_referenceAltitudeM = 0.0f;      ///< baro altitude averaged at startup [m]
        float m_referenceSumM = 0.0f;           ///< accumulator for the reference [m]
        std::uint32_t m_referenceCount = 0U;    ///< frames accumulated so far
        bool m_ready = false;                   ///< reference captured
    };
} // namespace mark4
