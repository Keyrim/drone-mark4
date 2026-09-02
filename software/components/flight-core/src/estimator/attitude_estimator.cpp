#include "flight_core/attitude_estimator.hpp"

#include <cmath>
#include <cstddef>

namespace mark4
{
    void AttitudeEstimator::update(const SensorFrame &frame, float dtS, bool allowAccelCorrection)
    {
        if (dtS <= 0.0f)
        {
            return; // first frame or gap: nothing may integrate
        }

        // Gravity direction error, only when the specific force is close
        // enough to 1 g to actually be the gravity reaction.
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        const float ax = frame.accelMps2[0];
        const float ay = frame.accelMps2[1];
        const float az = frame.accelMps2[2];
        const float gx = frame.gyroRadS[0];
        const float gy = frame.gyroRadS[1];
        const float gz = frame.gyroRadS[2];
        const float gyroNorm = std::sqrt(gx * gx + gy * gy + gz * gz);
        const float accelNorm = std::sqrt(ax * ax + ay * ay + az * az);
        if (allowAccelCorrection && gyroNorm < GYRO_QUIET_RADS &&
            std::fabs(accelNorm - GRAVITY_MPS2) < ACCEL_GATE_MPS2)
        {
            const float inv = 1.0f / accelNorm;
            const float vx = ax * inv;
            const float vy = ay * inv;
            const float vz = az * inv;

            // World up axis seen from the body frame, from the current estimate.
            const Quaternion &q = m_attitude;
            const float hx = 2.0f * (q.x * q.z - q.w * q.y);
            const float hy = 2.0f * (q.w * q.x + q.y * q.z);
            const float hz = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;

            // e = measured x estimated: zero when both directions agree.
            ex = vy * hz - vz * hy;
            ey = vz * hx - vx * hz;
            ez = vx * hy - vy * hx;

            // The bias only learns from small errors, and stays physically
            // plausible: reconvergence transients (crash, tumble) must not
            // be memorized as bias, they would misalign the next flights.
            if (ex * ex + ey * ey + ez * ez < KI_ERROR_GATE_SIN * KI_ERROR_GATE_SIN)
            {
                for (std::size_t axis = 0U; axis < 3U; ++axis)
                {
                    const float e = axis == 0U ? ex : (axis == 1U ? ey : ez);
                    float integral = m_integralFb[axis] + m_ki * e * dtS;
                    integral = integral > BIAS_LIMIT_RADS ? BIAS_LIMIT_RADS : integral;
                    integral = integral < -BIAS_LIMIT_RADS ? -BIAS_LIMIT_RADS : integral;
                    m_integralFb[axis] = integral;
                }
            }
        }

        const float wx = frame.gyroRadS[0] + m_integralFb[0] + m_kp * ex;
        const float wy = frame.gyroRadS[1] + m_integralFb[1] + m_kp * ey;
        const float wz = frame.gyroRadS[2] + m_integralFb[2] + m_kp * ez;

        // q <- normalize(q + 0.5 * q * (0, w) * dt)
        const Quaternion q = m_attitude;
        const float half = 0.5f * dtS;
        m_attitude.w += half * (-q.x * wx - q.y * wy - q.z * wz);
        m_attitude.x += half * (q.w * wx + q.y * wz - q.z * wy);
        m_attitude.y += half * (q.w * wy - q.x * wz + q.z * wx);
        m_attitude.z += half * (q.w * wz + q.x * wy - q.y * wx);

        const float norm = std::sqrt(m_attitude.w * m_attitude.w + m_attitude.x * m_attitude.x +
                                     m_attitude.y * m_attitude.y + m_attitude.z * m_attitude.z);
        const float invNorm = 1.0f / norm;
        m_attitude.w *= invNorm;
        m_attitude.x *= invNorm;
        m_attitude.y *= invNorm;
        m_attitude.z *= invNorm;

        refreshTelemetry();
    }

    void AttitudeEstimator::refreshTelemetry()
    {
        const Quaternion &q = m_attitude;
        // Same three expressions as the ground tools' eulerDeg(), in radians:
        // roll and yaw from atan2, pitch from an asin clamped to its domain
        // so a quaternion a hair past unit length cannot produce a NaN.
        float sinPitch = 2.0f * (q.w * q.y - q.z * q.x);
        sinPitch = sinPitch > 1.0f ? 1.0f : sinPitch;
        sinPitch = sinPitch < -1.0f ? -1.0f : sinPitch;
        m_eulerRad[0] =
            std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        m_eulerRad[1] = std::asin(sinPitch);
        m_eulerRad[2] =
            std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
        for (std::size_t axis = 0U; axis < m_gyroBiasRadS.size(); ++axis)
        {
            m_gyroBiasRadS[axis] = -m_integralFb[axis];
        }
    }
} // namespace mark4
