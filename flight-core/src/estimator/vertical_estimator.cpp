#include "flight_core/vertical_estimator.hpp"

#include <cmath>

namespace mark4
{
    namespace
    {
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

    void VerticalEstimator::update(const SensorFrame &frame, float dtS, const Quaternion &attitude)
    {
        const bool baroPlausible =
            frame.baroPa >= MIN_PLAUSIBLE_PA && frame.baroPa <= MAX_PLAUSIBLE_PA;
        const float baroAltitudeM = pressureAltitudeM(frame.baroPa);

        if (!m_ready)
        {
            // Only a resting drone may seed the reference: plausible baro,
            // specific force near 1 g, quiet gyro. A core booted mid-motion
            // (in-air reboot) simply completes the capture after landing.
            const float accelNorm = std::sqrt(frame.accelMps2[0] * frame.accelMps2[0] +
                                              frame.accelMps2[1] * frame.accelMps2[1] +
                                              frame.accelMps2[2] * frame.accelMps2[2]);
            const float gyroNorm = std::sqrt(frame.gyroRadS[0] * frame.gyroRadS[0] +
                                             frame.gyroRadS[1] * frame.gyroRadS[1] +
                                             frame.gyroRadS[2] * frame.gyroRadS[2]);
            const bool resting = std::fabs(accelNorm - GRAVITY_MPS2) < REFERENCE_ACCEL_GATE_MPS2 &&
                                 gyroNorm < REFERENCE_GYRO_QUIET_RADS;
            if (!baroPlausible || !resting)
            {
                return; // a faulty sensor or a moving drone must not seed it
            }
            m_referenceSumM += baroAltitudeM;
            ++m_referenceCount;
            if (m_referenceCount >= REFERENCE_SAMPLES)
            {
                m_referenceAltitudeM = m_referenceSumM / static_cast<float>(m_referenceCount);
                m_ready = true;
            }
            return;
        }

        if (dtS <= 0.0f)
        {
            return; // first frame or gap: nothing may integrate
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
        m_horizontalMps[0] += (accelWorldX - m_horizontalMps[0] / HORIZONTAL_LEAK_S) * dtS;
        m_horizontalMps[1] += (accelWorldY - m_horizontalMps[1] / HORIZONTAL_LEAK_S) * dtS;

        // Predict from the accelerometer, correct toward the baro altitude.
        m_velocityMps += verticalAccel * dtS;
        m_altitudeM += m_velocityMps * dtS;

        if (!baroPlausible)
        {
            return; // no correction: coast on the accelerometer alone
        }
        float errorM = (baroAltitudeM - m_referenceAltitudeM) - m_altitudeM;
        errorM = errorM > MAX_INNOVATION_M ? MAX_INNOVATION_M : errorM;
        errorM = errorM < -MAX_INNOVATION_M ? -MAX_INNOVATION_M : errorM;
        m_altitudeM += m_altitudeGain * errorM * dtS;
        m_velocityMps += m_velocityGain * errorM * dtS;
    }
} // namespace mark4
