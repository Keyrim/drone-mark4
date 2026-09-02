#include "platform_sim/plant_link.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <poll.h>

namespace mark4
{
    void PlantLink::OnPayload(void *context,
                              std::uint32_t src,
                              const std::uint8_t *payload,
                              std::size_t size)
    {
        auto &self = *static_cast<PlantLink *>(context);
        mark4_Envelope envelope;
        if (decodeEnvelope(payload, size, envelope) &&
            envelope.which_body == mark4_Envelope_sim_sensor_tag)
        {
            if (self.m_hasPending)
            {
                ++self.m_overruns;
            }
            self.m_pending = envelope.body.sim_sensor;
            self.m_pendingSrc = src;
            self.m_hasPending = true;
            return;
        }
        // Whatever is not a sensor message is a command, a beacon or a
        // message this build does not know: the composition root sorts
        // them out of the ring, exactly as on the board.
        self.m_commands.push(src, payload, size);
    }

    void PlantLink::poll()
    {
        m_transport.poll(m_clock.nowUs(), &PlantLink::OnPayload, this);
    }

    bool PlantLink::waitSensor(mark4_SimSensor &sensorOut,
                               std::uint32_t &srcOut,
                               std::uint64_t deadlineUs)
    {
        for (;;)
        {
            poll();
            if (m_hasPending)
            {
                sensorOut = m_pending;
                srcOut = m_pendingSrc;
                m_hasPending = false;
                return true;
            }
            const std::uint64_t nowUs = m_clock.nowUs();
            if (nowUs >= deadlineUs)
            {
                return false;
            }
            // Sleep on both sockets: the plant's unicasts land on the data
            // socket, the beacons of the LAN on the discovery one.
            std::array<pollfd, 2> fds = {pollfd{m_link.dataFd(), POLLIN, 0},
                                         pollfd{m_link.discoveryFd(), POLLIN, 0}};
            const auto remainingMs =
                static_cast<int>((deadlineUs - nowUs + US_PER_MS - 1U) / US_PER_MS);
            static_cast<void>(::poll(fds.data(), fds.size(), remainingMs));
        }
    }

    bool PlantLink::reply(const std::uint8_t *data, std::size_t size)
    {
        if (size <= m_lastReply.size())
        {
            std::memcpy(m_lastReply.data(), data, size);
            m_lastReplySize = size;
        }
        return send(data, size);
    }

    bool PlantLink::send(const std::uint8_t *data, std::size_t size)
    {
        return m_plant != 0U && m_transport.send(m_plant, data, size);
    }

    bool PlantLink::repeatLastReply()
    {
        return m_lastReplySize > 0U && send(m_lastReply.data(), m_lastReplySize);
    }
} // namespace mark4
