#pragma once

/// @file
/// @brief The USART1 rings (uart1.hpp) as the byte stream behind a
///        UartLink: the board's one transport link. The composition root
///        brings the USART up with uart1Init() before the first poll.

#include <cstddef>
#include <cstdint>

#include "platform_stm32/uart1.hpp"
#include "transport/uart_link.hpp"

namespace mark4
{
    class Uart1Stream final : public AbsByteStream
    {
      public:
        /// @brief Drains the DMA receive ring, never blocking.
        std::size_t read(std::uint8_t *bufferOut, std::size_t capacity) override
        {
            std::size_t count = 0U;
            while (count < capacity && uart1RxPop(bufferOut[count]))
            {
                ++count;
            }
            return count;
        }

        /// @brief Queues a whole frame on the transmit ring, or refuses it
        ///        whole: the transport then counts a drop instead of
        ///        blocking the flight loop on the wire.
        bool write(const std::uint8_t *data, std::size_t size) override
        {
            return uart1TxPush(data, size);
        }
    };
} // namespace mark4
