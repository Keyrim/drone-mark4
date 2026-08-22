#include "platform_sim/command_receiver_sim.hpp"

#include <cstddef>
#include <cstdint>

namespace mark4
{
    std::size_t CommandReceiverSim::poll(std::uint8_t *bufferOut, std::size_t capacity)
    {
        const std::size_t received = m_link.receiveNonBlocking(bufferOut, capacity);
        if (received > 0U)
        {
            ++m_packetsReceived;
        }
        return received;
    }
} // namespace mark4
