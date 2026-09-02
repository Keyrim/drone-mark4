#include "flight_core/vertical_controller.hpp"

namespace mark4
{
    float VerticalController::update(float setpointMps, float verticalVelocityMps, float dt)
    {
        const float error = setpointMps - verticalVelocityMps;

        m_integral += m_ki * error * dt;
        if (m_integral > INTEGRAL_LIMIT)
        {
            m_integral = INTEGRAL_LIMIT;
        }
        else if (m_integral < -INTEGRAL_LIMIT)
        {
            m_integral = -INTEGRAL_LIMIT;
        }

        const float pTerm = m_kp * error;
        float collective = m_hoverCollective + pTerm + m_integral;
        collective = collective < 0.0f ? 0.0f : collective;
        collective = collective > 1.0f ? 1.0f : collective;

        // The loop's own view of this step, for the telemetry registry: the
        // output is what really left, clamp included.
        m_setpointMps = setpointMps;
        m_measuredMps = verticalVelocityMps;
        m_errorMps = error;
        m_pTerm = pTerm;
        m_output = collective;
        return collective;
    }

    void VerticalController::reset()
    {
        m_integral = 0.0f;
        // The loop drives nothing while reset, so what it last computed is
        // no longer a measurement of it.
        m_setpointMps = 0.0f;
        m_measuredMps = 0.0f;
        m_errorMps = 0.0f;
        m_pTerm = 0.0f;
        m_output = 0.0f;
    }
} // namespace mark4
