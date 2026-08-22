#include "platform_stm32/telemetry_sender_stm32.hpp"

#include <cstdint>

#include "platform_stm32/uart1.hpp"
#include "protocol/serial_framing.hpp"

namespace mark4
{
    bool TelemetrySenderStm32::init()
    {
        return uart1Init();
    }

    void TelemetrySenderStm32::send(const std::uint8_t *data, std::size_t size)
    {
        std::uint8_t frame[SERIAL_MAX_PAYLOAD + SERIAL_FRAME_OVERHEAD];
        const std::size_t framed = encodeSerialFrame(data, size, frame);
        if (framed == 0U || !uart1TxPush(frame, framed))
        {
            ++m_packetsDropped;
            return;
        }
        ++m_packetsSent;
    }
} // namespace mark4
