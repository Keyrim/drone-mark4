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
        const std::array<float, 3> kp = {
            DEFAULT_KP_ROLL_PITCH, DEFAULT_KP_ROLL_PITCH, DEFAULT_KP_YAW};
        const std::array<float, 3> ki = {
            DEFAULT_KI_ROLL_PITCH, DEFAULT_KI_ROLL_PITCH, DEFAULT_KI_YAW};

        std::array<float, 3> torque{};
        for (std::size_t axis = 0U; axis < torque.size(); ++axis)
        {
            const float error = setpointRadS[axis] - gyroRadS[axis];
            m_integral[axis] = clampAbs(m_integral[axis] + ki[axis] * error * dt, INTEGRAL_LIMIT);
            torque[axis] = kp[axis] * error + m_integral[axis];
        }
        return torque;
    }

    void RateController::reset()
    {
        m_integral = {};
    }
} // namespace mark4
