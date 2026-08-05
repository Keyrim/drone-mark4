#include "flight_core/attitude_estimator.hpp"

#include <cmath>

namespace mark4
{
    namespace
    {
        constexpr float US_PER_S = 1e6f;
    } // namespace

    void AttitudeEstimator::update(const SensorFrame &frame)
    {
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

        // Gravity direction error, only when the specific force is close
        // enough to 1 g to actually be the gravity reaction.
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        const float ax = frame.accelMps2[0];
        const float ay = frame.accelMps2[1];
        const float az = frame.accelMps2[2];
        const float accelNorm = std::sqrt(ax * ax + ay * ay + az * az);
        if (std::fabs(accelNorm - GRAVITY_MPS2) < ACCEL_GATE_MPS2)
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

            m_integralFb[0] += m_ki * ex * dt;
            m_integralFb[1] += m_ki * ey * dt;
            m_integralFb[2] += m_ki * ez * dt;
        }

        const float wx = frame.gyroRadS[0] + m_integralFb[0] + m_kp * ex;
        const float wy = frame.gyroRadS[1] + m_integralFb[1] + m_kp * ey;
        const float wz = frame.gyroRadS[2] + m_integralFb[2] + m_kp * ez;

        // q <- normalize(q + 0.5 * q * (0, w) * dt)
        const Quaternion q = m_attitude;
        const float half = 0.5f * dt;
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
    }
} // namespace mark4
