#include "platform_sim/sensor_source_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/sim_link.hpp"
#include "protocol/version.hpp"

namespace mark4
{
    namespace
    {
        /// Larger than any expected packet, so an oversized datagram comes out
        /// with its real size instead of being truncated to a valid one.
        constexpr std::size_t RECEIVE_BUFFER_SIZE = 256U;
    } // namespace

    bool SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        std::array<std::uint8_t, RECEIVE_BUFFER_SIZE> wire{};

        while (true)
        {
            const std::size_t received = m_link.receive(wire.data(), wire.size());
            if (received == 0U)
            {
                return false; // idle link: the source is exhausted
            }
            if (received != mark4::SIM_SENSOR_PACKET_SIZE || wire[0] != mark4::PROTOCOL_VERSION)
            {
                continue; // not a sensor packet we understand: keep waiting
            }

            mark4::SimSensorPacket packet{};
            std::memcpy(&packet, wire.data(), sizeof(packet));

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
            frameOut.rc.killSwitch = packet.killSwitch != 0U;
            frameOut.rc.throttle = packet.throttle;
            return true;
        }
    }
} // namespace mark4
