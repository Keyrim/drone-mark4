#include "platform_stm32/command_receiver_stm32.hpp"

#include <cstdint>

#include "platform_stm32/uart1.hpp"

namespace mark4
{
    bool CommandReceiverStm32::init()
    {
        return uart1Init();
    }

    std::size_t CommandReceiverStm32::poll(std::uint8_t *bufferOut, std::size_t capacity)
    {
        std::uint8_t byte = 0U;
        while (uart1RxPop(byte))
        {
            const std::size_t size = m_parser.feed(byte);
            if (size == 0U || size > capacity)
            {
                continue;
            }
            const std::uint8_t *payload = m_parser.payload();
            for (std::size_t index = 0U; index < size; ++index)
            {
                bufferOut[index] = payload[index];
            }
            ++m_packetsReceived;
            return size;
        }
        return 0U;
    }
} // namespace mark4
