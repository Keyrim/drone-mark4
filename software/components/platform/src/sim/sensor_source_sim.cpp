#include "platform_sim/sensor_source_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/header.hpp"
#include "protocol/sim_link.hpp"

namespace mark4
{
    namespace
    {
        /// Larger than any expected packet, so an oversized datagram comes out
        /// with its real size instead of being truncated to a valid one.
        constexpr std::size_t RECEIVE_BUFFER_SIZE = 256U;
    } // namespace

    FrameWait SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        std::array<std::uint8_t, RECEIVE_BUFFER_SIZE> wire{};

        while (true)
        {
            const std::size_t received = m_link.receive(wire.data(), wire.size());
            if (received == 0U)
            {
                return FrameWait::TIMEOUT; // idle link: the caller decides
            }
            if (received != mark4::SIM_SENSOR_PACKET_SIZE ||
                !mark4::hasHeader(wire.data(), received, mark4::PacketType::SIM_SENSOR))
            {
                continue; // not a sensor packet we understand: keep waiting
            }

            mark4::SimSensorPacket packet{};
            std::memcpy(&packet, wire.data(), sizeof(packet));

            if (packet.sessionId != m_sessionId)
            {
                /* A different plant: its simulated clock starts over, so
                   nothing this source remembers about timestamps applies to
                   it any more. */
                m_timestampSeen = false;
            }

            if (m_timestampSeen && packet.timestampUs == m_lastTimestampUs)
            {
                /* The simulator is asking again for the answer to a tick it
                   already sent: the reply was lost, not the sample. Answering
                   twice is correct, stepping twice would fabricate a frame
                   the plant never produced. */
                m_link.acceptLastSender();
                static_cast<void>(m_link.repeatLastReply());
                ++m_duplicateFrames;
                continue;
            }

            frameOut.timestampUs = packet.timestampUs;
            /* The std::array members sit at odd offsets in the packed struct:
               reading them through it would bind a reference to a misaligned
               address, so they are copied straight out of the datagram. */
            std::memcpy(frameOut.gyroRadS.data(),
                        wire.data() + offsetof(mark4::SimSensorPacket, gyroRadS),
                        sizeof(frameOut.gyroRadS));
            std::memcpy(frameOut.accelMps2.data(),
                        wire.data() + offsetof(mark4::SimSensorPacket, accelMps2),
                        sizeof(frameOut.accelMps2));
            frameOut.baroPa = packet.baroPa;
            // The RC fields of the frame are deliberately left alone: RC is
            // not a sensor reading, and the composition root grafts it from
            // its RcTracker after this call returns.
            m_resetCount = packet.resetCount;
            m_sessionId = packet.sessionId;
            m_lockstepTimeouts = packet.lockstepTimeouts;
            m_lastTimestampUs = packet.timestampUs;
            m_timestampSeen = true;
            // Only a validated sensor packet may steer the motor replies:
            // a stray datagram on the port must not stall the lockstep.
            m_link.acceptLastSender();
            return FrameWait::FRAME;
        }
    }
} // namespace mark4
