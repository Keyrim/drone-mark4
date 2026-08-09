#include "platform_sim/motor_sink_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/header.hpp"
#include "protocol/sim_link.hpp"

namespace mark4
{
    void MotorSinkSim::push(const mark4::ActuatorFrame &frame)
    {
        m_last = frame;
        ++m_pushCount;

        mark4::SimActuatorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_ACTUATOR);
        packet.echoTimestampUs = frame.timestampUs;

        std::array<std::uint8_t, sizeof(packet)> wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));
        /* The motor array sits at an odd offset in the packed struct: writing
           it through the struct would bind a reference to a misaligned
           address, so it goes straight into the datagram bytes. */
        std::memcpy(wire.data() + offsetof(mark4::SimActuatorPacket, motor),
                    frame.motor.data(),
                    sizeof(frame.motor));
        /* Same reason for the scenario block: it is a packed struct sitting
           at an odd offset, so it goes in as bytes and never through a
           reference to one of its fields. */
        std::memcpy(wire.data() + offsetof(mark4::SimActuatorPacket, scenario),
                    &m_scenario,
                    sizeof(m_scenario));
        static_cast<void>(m_link.replyToLastSender(wire.data(), wire.size()));
    }
} // namespace mark4
