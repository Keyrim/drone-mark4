#include "flight_core/throw_detector.hpp"

#include <cmath>

namespace mark4
{
    namespace
    {
        constexpr float US_PER_S = 1e6f;

        /// Denominator of the ballistic apex height vz^2 / 2g.
        constexpr float TWO_G_MPS2 = 2.0f * GRAVITY_MPS2;
    } // namespace

    void ThrowDetector::reset()
    {
        m_state = ThrowState::IDLE;
        m_thrustStartUs = 0U;
        m_thrustLastUs = 0U;
        m_inFreeFallStreak = false;
        m_inExitStreak = false;
    }

    void ThrowDetector::update(const SensorFrame &frame, float verticalVelocityMps, float altitudeM)
    {
        const float ax = frame.accelMps2[0];
        const float ay = frame.accelMps2[1];
        const float az = frame.accelMps2[2];
        const float norm = std::sqrt(ax * ax + ay * ay + az * az);
        const std::uint64_t now = frame.timestampUs;

        switch (m_state)
        {
            case ThrowState::IDLE:
                if (norm >= THRUST_THRESHOLD_MPS2)
                {
                    m_state = ThrowState::THRUST;
                    m_thrustStartUs = now;
                    m_thrustLastUs = now;
                    m_inFreeFallStreak = false;
                }
                break;

            case ThrowState::THRUST:
                if (norm >= THRUST_THRESHOLD_MPS2)
                {
                    m_thrustLastUs = now;
                    m_inFreeFallStreak = false;
                }
                else if (norm < FREE_FALL_THRESHOLD_MPS2)
                {
                    if (!m_inFreeFallStreak)
                    {
                        // Start of a free fall streak: this is the release
                        // candidate, freeze the vertical state right here.
                        m_inFreeFallStreak = true;
                        m_freeFallStartUs = now;
                        m_candidateVelocityMps = verticalVelocityMps;
                        m_candidateAltitudeM = altitudeM;
                    }
                    else if (now - m_freeFallStartUs >= FREE_FALL_CONFIRM_US)
                    {
                        const bool thrustLongEnough =
                            m_thrustLastUs - m_thrustStartUs >= THRUST_MIN_US;
                        const bool fastEnough = m_candidateVelocityMps >= MIN_RELEASE_VELOCITY_MPS;
                        if (thrustLongEnough && fastEnough)
                        {
                            ++m_throwCount;
                            m_releaseVelocityMps = m_candidateVelocityMps;
                            m_releaseTimestampUs = m_freeFallStartUs;
                            m_apexTimestampUs = m_freeFallStartUs +
                                                static_cast<std::uint64_t>(m_candidateVelocityMps /
                                                                           GRAVITY_MPS2 * US_PER_S);
                            m_apexAltitudeM = m_candidateAltitudeM + m_candidateVelocityMps *
                                                                         m_candidateVelocityMps /
                                                                         TWO_G_MPS2;
                            m_state = ThrowState::BALLISTIC;
                            m_inExitStreak = false;
                        }
                        else
                        {
                            reset(); // incomplete signature: never arm on it
                        }
                    }
                }
                else
                {
                    // Between the two thresholds: transition after the hand opens,
                    // or just a move. The free fall streak is broken; give up if
                    // the release does not come.
                    m_inFreeFallStreak = false;
                    if (now - m_thrustLastUs >= RELEASE_TIMEOUT_US)
                    {
                        reset();
                    }
                }
                break;

            case ThrowState::BALLISTIC:
                if (norm >= BALLISTIC_EXIT_MPS2)
                {
                    if (!m_inExitStreak)
                    {
                        m_inExitStreak = true;
                        m_exitStartUs = now;
                    }
                    else if (now - m_exitStartUs >= BALLISTIC_EXIT_CONFIRM_US)
                    {
                        reset(); // caught, landed or crashed: the flight is over
                    }
                }
                else
                {
                    m_inExitStreak = false;
                }
                break;
        }
    }

    float ThrowDetector::ReadState(const void *context)
    {
        return static_cast<float>(static_cast<const ThrowDetector *>(context)->m_state);
    }

    float ThrowDetector::ReadCount(const void *context)
    {
        return static_cast<float>(static_cast<const ThrowDetector *>(context)->m_throwCount);
    }

    float ThrowDetector::ReadApexTime(const void *context)
    {
        return static_cast<float>(static_cast<const ThrowDetector *>(context)->m_apexTimestampUs);
    }
} // namespace mark4
