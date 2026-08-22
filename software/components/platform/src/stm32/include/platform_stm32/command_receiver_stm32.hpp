#pragma once

/// @file
/// @brief Command uplink over the shared USART1 transport.

#include <cstddef>
#include <cstdint>

#include "platform/command_receiver.hpp"
#include "protocol/serial_framing.hpp"

namespace mark4
{
    /// Receives protocol/ packets from the PC on the shared USART1
    /// transport (uart1.hpp): the receive ring is drained through the
    /// serial framing parser, one complete payload comes out per poll()
    /// at most. Decoding belongs to the consumer, per the interface.
    class CommandReceiverStm32 final : public AbsCommandReceiver
    {
      public:
        /// @brief Brings the shared USART1 transport up.
        /// @return true, kept bool for the app init contract
        bool init();

        /// @brief Drains the receive ring into the framing parser.
        /// @param[out] bufferOut destination, valid only when returning > 0
        /// @param capacity size of the destination buffer in bytes
        /// @return payload size in bytes, or 0 when no complete frame came
        ///         out (a payload larger than capacity is dropped whole)
        std::size_t poll(std::uint8_t *bufferOut, std::size_t capacity) override;

        /// @return complete frames handed out since construction
        [[nodiscard]] std::uint32_t packetsReceived() const
        {
            return m_packetsReceived;
        }

      private:
        SerialFrameParser m_parser;           ///< incremental frame decoder
        std::uint32_t m_packetsReceived = 0U; ///< frames handed to the caller
    };
} // namespace mark4
