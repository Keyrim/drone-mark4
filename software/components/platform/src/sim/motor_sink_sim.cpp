#include "platform_sim/motor_sink_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/envelope.hpp"

namespace mark4
{
    void MotorSinkSim::push(const mark4::ActuatorFrame &frame)
    {
        m_last = frame;
        ++m_pushCount;

        if (m_scenarioPending && emitScenario(m_pendingScenario))
        {
            m_scenarioPending = false;
        }

        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_sim_actuator_tag;
        envelope.body.sim_actuator.echo_timestamp_us = frame.timestampUs;
        std::memcpy(envelope.body.sim_actuator.motor,
                    frame.motor.data(),
                    sizeof(envelope.body.sim_actuator.motor));

        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> wire{};
        std::size_t size = 0U;
        if (encodeEnvelope(envelope, wire.data(), wire.size(), size))
        {
            static_cast<void>(m_link.reply(wire.data(), size));
        }
    }

    void MotorSinkSim::sendScenario(const mark4_SimScenario &scenario)
    {
        m_pendingScenario = scenario;
        m_scenarioPending = !emitScenario(scenario);
    }

    bool MotorSinkSim::emitScenario(const mark4_SimScenario &scenario)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_sim_scenario_tag;
        envelope.body.sim_scenario = scenario;
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> wire{};
        std::size_t size = 0U;
        return encodeEnvelope(envelope, wire.data(), wire.size(), size) &&
               m_link.send(wire.data(), size);
    }
} // namespace mark4
