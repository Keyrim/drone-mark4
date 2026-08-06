#include "flight_core/vertical_estimator.hpp"

#include <cmath>

namespace mark4
{
    namespace
    {
        constexpr float US_PER_S = 1e6f;

        // Standard atmosphere, inverse of pressure(altitude); the constants
        // mirror the simulator's sensor model.
        constexpr float SEA_LEVEL_PRESSURE_PA = 101325.0f;
        constexpr float ATMOSPHERE_LAPSE_PER_M = 2.25577e-5f;
        constexpr float ATMOSPHERE_EXPONENT_INV = 1.0f / 5.25588f;

        /// Below this pressure the standard formula leaves its valid domain.
        constexpr float MIN_PRESSURE_PA = 1000.0f;
    } // namespace

    float VerticalEstimator::pressureAltitudeM(float pressurePa)
    {
        const float clampedPa = pressurePa < MIN_PRESSURE_PA ? MIN_PRESSURE_PA : pressurePa;
        const float ratio = clampedPa / SEA_LEVEL_PRESSURE_PA;
        return (1.0f - std::pow(ratio, ATMOSPHERE_EXPONENT_INV)) / ATMOSPHERE_LAPSE_PER_M;
    }

    void VerticalEstimator::update(const SensorFrame &frame, const Quaternion &attitude)
    {
        const float baroAltitudeM = pressureAltitudeM(frame.baroPa);

        if (!m_ready)
        {
            m_referenceSumM += baroAltitudeM;
            ++m_referenceCount;
            if (m_referenceCount >= REFERENCE_SAMPLES)
            {
                m_referenceAltitudeM = m_referenceSumM / static_cast<float>(m_referenceCount);
                m_ready = true;
            }
            m_prevTimestampUs = frame.timestampUs;
            m_hasPrevTimestamp = true;
            return;
        }

        if (!m_hasPrevTimestamp || frame.timestampUs <= m_prevTimestampUs)
        {
            m_prevTimestampUs = frame.timestampUs;
            m_hasPrevTimestamp = true;
            return;
        }
        const float dt = static_cast<float>(frame.timestampUs - m_prevTimestampUs) / US_PER_S;
        m_prevTimestampUs = frame.timestampUs;
        if (dt > MAX_STEP_S)
        {
            return; // gap in the stream: reference re-armed, nothing integrated
        }

        // Specific force in the world frame (body-to-world rotation). On the
        // vertical, minus gravity: 0 at rest, -g in free fall. Gravity has no
        // horizontal component, so x and y integrate as they are.
        const Quaternion &q = attitude;
        const float ax = frame.accelMps2[0];
        const float ay = frame.accelMps2[1];
        const float az = frame.accelMps2[2];
        const float accelWorldX = (1.0f - 2.0f * (q.y * q.y + q.z * q.z)) * ax +
                                  2.0f * (q.x * q.y - q.w * q.z) * ay +
                                  2.0f * (q.x * q.z + q.w * q.y) * az;
        const float accelWorldY = 2.0f * (q.x * q.y + q.w * q.z) * ax +
                                  (1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * ay +
                                  2.0f * (q.y * q.z - q.w * q.x) * az;
        const float accelWorldZ = 2.0f * (q.x * q.z - q.w * q.y) * ax +
                                  2.0f * (q.y * q.z + q.w * q.x) * ay +
                                  (1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * az;
        const float verticalAccel = accelWorldZ - GRAVITY_MPS2;

        // Dead reckoning, leaking toward zero: nothing measures the
        // horizontal velocity, the leak bounds the integration drift.
        m_horizontalMps[0] += (accelWorldX - m_horizontalMps[0] / HORIZONTAL_LEAK_S) * dt;
        m_horizontalMps[1] += (accelWorldY - m_horizontalMps[1] / HORIZONTAL_LEAK_S) * dt;

        // Predict from the accelerometer, correct toward the baro altitude.
        m_velocityMps += verticalAccel * dt;
        m_altitudeM += m_velocityMps * dt;

        const float errorM = (baroAltitudeM - m_referenceAltitudeM) - m_altitudeM;
        m_altitudeM += m_altitudeGain * errorM * dt;
        m_velocityMps += m_velocityGain * errorM * dt;
    }
} // namespace mark4
