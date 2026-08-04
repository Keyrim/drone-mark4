#include "platform_sim/telemetry_sender_sim.hpp"

namespace mark4
{
    void TelemetrySenderSim::send(const std::uint8_t *data, std::size_t size)
    {
        static_cast<void>(data);
        ++m_packetCount;
        m_byteCount += size;
    }
} // namespace mark4
