#pragma once

/// @file
/// @brief Throw detection and apex prediction.

#include <cstdint>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Phase of the throw detector.
    enum class ThrowState : std::uint8_t
    {
        IDLE = 0U,      ///< nothing resembling a throw in progress
        THRUST = 1U,    ///< sustained hand thrust seen, waiting for the release
        BALLISTIC = 2U, ///< free fall confirmed: a throw was detected
    };

    /// Detects a hand throw from the accelerometer norm and predicts the apex.
    ///
    /// A complete signature is required, in order: a sustained thrust phase
    /// (several g while the hand accelerates the drone), then confirmed free
    /// fall (near 0 g). A drop without thrust, a short bump or a hand carry
    /// never detects - by design, this is the safety gate before anything
    /// ever spins.
    ///
    /// The release velocity is not integrated here: it is read from the
    /// vertical estimator at the instant the specific force vanishes (the
    /// release), and the apex is predicted from it - never detected after
    /// the fact: t_apex = t_release + vz0 / g.
    class ThrowDetector
    {
      public:
        /// Accel norm above which the hand is considered thrusting.
        static constexpr float THRUST_THRESHOLD_MPS2 = 2.0f * GRAVITY_MPS2;

        /// Accel norm below which the drone is considered in free fall.
        static constexpr float FREE_FALL_THRESHOLD_MPS2 = 0.3f * GRAVITY_MPS2;

        /// Accel norm above which the ballistic phase is considered over.
        static constexpr float BALLISTIC_EXIT_MPS2 = 0.5f * GRAVITY_MPS2;

        /// Minimum time the norm must stay above the thrust threshold.
        static constexpr std::uint64_t THRUST_MIN_US = 40000U;

        /// Free fall must last this long before the throw is confirmed.
        static constexpr std::uint64_t FREE_FALL_CONFIRM_US = 50000U;

        /// Give up waiting for the release this long after the thrust ends.
        static constexpr std::uint64_t RELEASE_TIMEOUT_US = 300000U;

        /// The exit norm must last this long before returning to idle.
        static constexpr std::uint64_t BALLISTIC_EXIT_CONFIRM_US = 100000U;

        /// Upward release velocity below which the signature is rejected: a
        /// real hand throw leaves at several m/s.
        static constexpr float MIN_RELEASE_VELOCITY_MPS = 2.0f;

        /// @brief Advances the detector with one sensor frame.
        /// @param frame sensor frame carrying accel and the timestamp
        /// @param verticalVelocityMps current vertical velocity estimate [m/s]
        /// @param altitudeM current altitude estimate [m]
        void update(const SensorFrame &frame, float verticalVelocityMps, float altitudeM);

        /// @return current phase
        [[nodiscard]] ThrowState state() const
        {
            return m_state;
        }

        /// @return number of throws detected since construction
        [[nodiscard]] std::uint32_t throwCount() const
        {
            return m_throwCount;
        }

        /// @return upward velocity at the release of the last throw [m/s]
        [[nodiscard]] float releaseVelocityMps() const
        {
            return m_releaseVelocityMps;
        }

        /// @return timestamp of the release of the last throw [us]
        [[nodiscard]] std::uint64_t releaseTimestampUs() const
        {
            return m_releaseTimestampUs;
        }

        /// @return predicted apex instant of the last throw [us]
        [[nodiscard]] std::uint64_t apexTimestampUs() const
        {
            return m_apexTimestampUs;
        }

        /// @return predicted apex altitude of the last throw [m]
        [[nodiscard]] float apexAltitudeM() const
        {
            return m_apexAltitudeM;
        }

      private:
        void reset();

        ThrowState m_state = ThrowState::IDLE; ///< current phase

        std::uint64_t m_thrustStartUs = 0U;   ///< first frame above the thrust threshold
        std::uint64_t m_thrustLastUs = 0U;    ///< last frame above the thrust threshold
        std::uint64_t m_freeFallStartUs = 0U; ///< start of the free fall streak, 0 = none
        std::uint64_t m_exitStartUs = 0U;     ///< start of the ballistic exit streak, 0 = none
        float m_candidateVelocityMps = 0.0f;  ///< vertical velocity at the streak start
        float m_candidateAltitudeM = 0.0f;    ///< altitude at the streak start

        std::uint32_t m_throwCount = 0U;         ///< throws detected so far
        float m_releaseVelocityMps = 0.0f;       ///< last release velocity [m/s]
        std::uint64_t m_releaseTimestampUs = 0U; ///< last release instant [us]
        std::uint64_t m_apexTimestampUs = 0U;    ///< last predicted apex instant [us]
        float m_apexAltitudeM = 0.0f;            ///< last predicted apex altitude [m]
    };
} // namespace mark4
