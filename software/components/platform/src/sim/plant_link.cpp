#include "platform_sim/plant_link.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <poll.h>

namespace mark4
{
    bool PlantLink::onMessage(std::uint32_t src,
                              const mark4_Envelope &envelope,
                              std::uint64_t nowUs)
    {
        static_cast<void>(nowUs);
        if (m_hasPending)
        {
            ++m_overruns;
        }
        m_pending = envelope.body.sim_sensor;
        m_pendingSrc = src;
        m_hasPending = true;
        return true;
    }

    void PlantLink::poll()
    {
        m_messenger.poll(m_clock.nowUs());
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
            // socket, the broadcasts of the LAN on the discovery one.
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
