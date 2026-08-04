#include "platform_sim/motor_sink_sim.hpp"

namespace mark4
{
    void MotorSinkSim::push(const mark4::ActuatorFrame &frame)
    {
        m_last = frame;
        ++m_pushCount;
    }
} // namespace mark4
