#pragma once

/// @file
/// @brief Vertical state estimator (complementary filter, baro + accel).

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
    /// REFERENCE_SAMPLES frames: the estimator reads zero where it woke up.
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

        /// Integration steps longer than this are treated as gaps in the
        /// stream: the step is skipped instead of being integrated.
        static constexpr float MAX_STEP_S = 0.05f;

        /// @param altitudeGain correction gain on the altitude [1/s]
        /// @param velocityGain correction gain on the velocity [1/s^2]
        explicit VerticalEstimator(float altitudeGain = DEFAULT_ALTITUDE_GAIN,
                                   float velocityGain = DEFAULT_VELOCITY_GAIN)
            : m_altitudeGain(altitudeGain),
              m_velocityGain(velocityGain)
        {
        }

        /// @brief Advances the estimate with one sensor frame. The first
        ///        REFERENCE_SAMPLES frames only build the baro reference; the
        ///        integration step is the timestamp delta between consecutive
        ///        frames and gaps larger than MAX_STEP_S only re-arm it.
        /// @param frame sensor frame carrying accel, baro and the timestamp
        /// @param attitude current body-to-world attitude estimate
        void update(const SensorFrame &frame, const Quaternion &attitude);

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

        /// @brief Standard atmosphere altitude for a static pressure.
        /// @param pressurePa static pressure [Pa], clamped to a sane range
        /// @return absolute altitude above the standard sea level [m]
        static float pressureAltitudeM(float pressurePa);

      private:
        float m_altitudeGain;                 ///< altitude correction gain [1/s]
        float m_velocityGain;                 ///< velocity correction gain [1/s^2]
        float m_altitudeM = 0.0f;             ///< estimate, relative to the reference [m]
        float m_velocityMps = 0.0f;           ///< estimate, positive up [m/s]
        float m_referenceAltitudeM = 0.0f;    ///< baro altitude averaged at startup [m]
        float m_referenceSumM = 0.0f;         ///< accumulator for the reference [m]
        std::uint32_t m_referenceCount = 0U;  ///< frames accumulated so far
        bool m_ready = false;                 ///< reference captured
        std::uint64_t m_prevTimestampUs = 0U; ///< integration reference [us]
        bool m_hasPrevTimestamp = false;      ///< false until the first frame
    };
} // namespace mark4
