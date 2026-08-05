#include "flight_core/vertical_controller.hpp"

namespace mark4
{
    float VerticalController::update(float setpointMps, float verticalVelocityMps, float dt)
    {
        const float error = setpointMps - verticalVelocityMps;

        m_integral += DEFAULT_KI * error * dt;
        if (m_integral > INTEGRAL_LIMIT)
        {
            m_integral = INTEGRAL_LIMIT;
        }
        else if (m_integral < -INTEGRAL_LIMIT)
        {
            m_integral = -INTEGRAL_LIMIT;
        }

        const float collective = m_hoverCollective + DEFAULT_KP * error + m_integral;
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
