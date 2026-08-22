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

        const float collective = m_hoverCollective + m_kp * error + m_integral;
        if (collective < 0.0f)
        {
            return 0.0f;
        }
        return collective > 1.0f ? 1.0f : collective;
    }

    void VerticalController::reset()
    {
        m_integral = 0.0f;
    }
} // namespace mark4
