#pragma once

/// @file
/// @brief Telemetry over UART1, through the FTDI dongle to the PC.

#include <cstddef>
#include <cstdint>

#include "platform/telemetry_sender.hpp"

namespace mark4
{
    /// Sends protocol/ packets on the shared USART1 transport (uart1.hpp),
    /// each wrapped in a serial frame (protocol/serial_framing.hpp) so the
    /// receiver can find packet boundaries in the byte stream. send() only
    /// copies bytes and never waits on the wire. A packet that does not
    /// fit is dropped whole and counted, keeping frames self-consistent.
    class TelemetrySenderStm32 final : public AbsTelemetrySender
    {
      public:
        /// @brief Brings the shared USART1 transport up.
        /// @return true, kept bool for the app init contract
        bool init();

        /// @brief Frames and queues one packet, never blocking.
        /// @param data packet bytes
        /// @param size packet size in bytes
        void send(const std::uint8_t *data, std::size_t size) override;

        /// @return packets queued since construction
        [[nodiscard]] std::uint32_t packetsSent() const
        {
            return m_packetsSent;
        }

        /// @return packets dropped because the ring was full or the size
        ///         did not fit a serial frame
        [[nodiscard]] std::uint32_t packetsDropped() const
        {
            return m_packetsDropped;
        }

      private:
        std::uint32_t m_packetsSent = 0U;    ///< packets queued on the ring
        std::uint32_t m_packetsDropped = 0U; ///< packets refused whole
    };
} // namespace mark4
