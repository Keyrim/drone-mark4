#include "flight_core/rate_controller.hpp"

namespace mark4
{
    namespace
    {
        /// @return value clamped to [-limit, limit]
        float clampAbs(float value, float limit)
        {
            if (value < -limit)
            {
                return -limit;
            }
            return value > limit ? limit : value;
        }
    } // namespace

    std::array<float, 3> RateController::update(const std::array<float, 3> &setpointRadS,
                                                const std::array<float, 3> &gyroRadS,
                                                float dt)
    {
        std::array<float, 3> torque{};
        for (std::size_t axis = 0U; axis < torque.size(); ++axis)
        {
            const float error = setpointRadS[axis] - gyroRadS[axis];
            m_integral[axis] = clampAbs(m_integral[axis] + m_ki[axis] * error * dt, INTEGRAL_LIMIT);
            const float pTerm = m_kp[axis] * error;
            torque[axis] = pTerm + m_integral[axis];

            // The loop's own view of this step, for the telemetry registry.
            m_setpointRadS[axis] = setpointRadS[axis];
            m_measuredRadS[axis] = gyroRadS[axis];
            m_errorRadS[axis] = error;
            m_pTerm[axis] = pTerm;
            m_output[axis] = torque[axis];
        }
        return torque;
    }

    void RateController::reset()
    {
        m_integral = {};
        // The loop stops driving anything, so what it last computed is no
        // longer a measurement of it: a held curve would read as a live one.
        m_setpointRadS = {};
        m_measuredRadS = {};
        m_errorRadS = {};
        m_pTerm = {};
        m_output = {};
    }
} // namespace mark4
