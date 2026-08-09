#pragma once

/// @file
/// @brief Broadcasting telemetry sender for the sim variant.

#include <cstddef>
#include <cstdint>

#include "platform/telemetry_sender.hpp"

namespace mark4
{
    /// Broadcasts telemetry packets over UDP, to any number of listeners. Owns
    /// its own socket: it only ever sends, so it needs no bound port and no
    /// shared link. The socket is closed by the destructor.
    class TelemetrySenderSim final : public AbsTelemetrySender
    {
      public:
        TelemetrySenderSim() = default;
        ~TelemetrySenderSim() override;

        TelemetrySenderSim(const TelemetrySenderSim &) = delete;
        TelemetrySenderSim &operator=(const TelemetrySenderSim &) = delete;
        TelemetrySenderSim(TelemetrySenderSim &&) = delete;
        TelemetrySenderSim &operator=(TelemetrySenderSim &&) = delete;

        /// @brief Creates the broadcast socket.
        /// @param port destination port of the broadcast; the mirror copy
        ///        goes to telemetryMirrorPort(port). Configurable so several
        ///        instances can run side by side in a batch campaign.
        /// @param mirror false for a stream that has no mirror consumer, so
        ///        the port at telemetryMirrorPort(port) never sees traffic
        ///        that does not belong to it
        /// @return true when the socket is ready to send
        bool open(std::uint16_t port, bool mirror = true);

        /// @brief Broadcasts one packet. Best effort: a send failure is logged
        ///        once and the packet is dropped.
        /// @param data packet bytes
        /// @param size packet size in bytes
        void send(const std::uint8_t *data, std::size_t size) override;

        /// @return number of packets sent since construction
        [[nodiscard]] std::uint32_t packetCount() const
        {
            return m_packetCount;
        }

        /// @return total bytes sent since construction
        [[nodiscard]] std::size_t byteCount() const
        {
            return m_byteCount;
        }

      private:
        int m_socketFd = -1;              ///< -1 when closed
        std::uint16_t m_port = 0U;        ///< broadcast destination port
        bool m_mirror = true;             ///< also send to the mirror port
        std::uint32_t m_packetCount = 0U; ///< packets actually sent
        std::size_t m_byteCount = 0U;     ///< bytes actually sent
        bool m_sendFailureLogged = false; ///< keeps a broken link from flooding stderr
    };
} // namespace mark4
