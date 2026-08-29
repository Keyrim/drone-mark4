#pragma once

/// @file
/// @brief Telemetry sender over the transport: every packet is broadcast
///        to every node, which is what a stream any number of ground tools
///        may watch wants.

#include <cstddef>
#include <cstdint>

#include "platform/telemetry_sender.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    class TelemetrySenderTransport final : public AbsTelemetrySender
    {
      public:
        /// @param transport transport the packets leave by, owned by the
        ///        composition root
        explicit TelemetrySenderTransport(Transport &transport)
            : m_transport(transport)
        {
        }

        /// @brief Broadcasts one packet. Best effort: a frame a link could
        ///        not take (a full UART ring) is dropped and counted.
        /// @param data packet bytes
        /// @param size packet size in bytes
        void send(const std::uint8_t *data, std::size_t size) override
        {
            if (m_transport.send(BROADCAST_NODE, data, size))
            {
                ++m_packetCount;
                m_byteCount += size;
            }
            else
            {
                ++m_dropCount;
            }
        }

        /// @return packets refused by a link since construction
        [[nodiscard]] std::uint32_t dropCount() const
        {
            return m_dropCount;
        }

        /// @return packets sent since construction
        [[nodiscard]] std::uint32_t packetCount() const
        {
            return m_packetCount;
        }

        /// @return payload bytes sent since construction
        [[nodiscard]] std::size_t byteCount() const
        {
            return m_byteCount;
        }

      private:
        Transport &m_transport;           ///< output, not owned
        std::uint32_t m_packetCount = 0U; ///< packets actually sent
        std::uint32_t m_dropCount = 0U;   ///< packets refused by a link
        std::size_t m_byteCount = 0U;     ///< bytes actually sent
    };
} // namespace mark4
