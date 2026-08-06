#include "platform_sim/motor_sink_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/sim_link.hpp"
#include "protocol/version.hpp"

namespace mark4
{
    void MotorSinkSim::push(const mark4::ActuatorFrame &frame)
    {
        m_last = frame;
        ++m_pushCount;

        mark4::SimActuatorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.echoTimestampUs = frame.timestampUs;

        std::array<std::uint8_t, sizeof(packet)> wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));
        /* The motor array sits at an odd offset in the packed struct: writing
           it through the struct would bind a reference to a misaligned
           address, so it goes straight into the datagram bytes. */
        std::memcpy(wire.data() + offsetof(mark4::SimActuatorPacket, motor),
                    frame.motor.data(),
                    sizeof(frame.motor));
        static_cast<void>(m_link.replyToLastSender(wire.data(), wire.size()));
    }
} // namespace mark4
