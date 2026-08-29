#pragma once

/// @file
/// @brief Byte stream over one connected UDP socket, POSIX only: the way a
///        UartLink reaches a UART that a transparent bridge (the ESP32)
///        carries inside datagrams. Datagram boundaries mean nothing to the
///        serial framing, so a datagram is simply the next bytes of the
///        stream, and every write leaves as one datagram.

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/uart_link.hpp"

namespace mark4
{
    class UdpByteStream final : public AbsByteStream
    {
      public:
        /// Largest datagram taken whole. The bridge gathers at most 1024
        /// UART bytes per datagram; a frame from this side is under 600.
        static constexpr std::size_t MAX_DATAGRAM = 2048U;

        UdpByteStream() = default;
        UdpByteStream(const UdpByteStream &) = delete;
        UdpByteStream &operator=(const UdpByteStream &) = delete;
        UdpByteStream(UdpByteStream &&) = delete;
        UdpByteStream &operator=(UdpByteStream &&) = delete;
        ~UdpByteStream() override;

        /// @brief Opens a non-blocking socket connected to one peer. An open
        ///        stream is closed first.
        /// @param host peer IPv4 address, dotted quad
        /// @param port peer UDP port
        /// @return true when the socket is connected
        bool open(const char *host, std::uint16_t port);

        /// @brief Closes the socket, if open. read() then returns 0 and
        ///        write() false.
        void close();

        /// @return true while the socket is open
        [[nodiscard]] bool isOpen() const
        {
            return m_fd >= 0;
        }

        /// @return descriptor to poll(2) on, -1 when closed
        [[nodiscard]] int fd() const
        {
            return m_fd;
        }

        std::size_t read(std::uint8_t *bufferOut, std::size_t capacity) override;
        bool write(const std::uint8_t *data, std::size_t size) override;

      private:
        int m_fd = -1;                                      ///< connected socket, -1 = closed
        std::array<std::uint8_t, MAX_DATAGRAM> m_pending{}; ///< last datagram received
        std::size_t m_pendingSize = 0U;                     ///< bytes in m_pending
        std::size_t m_pendingIndex = 0U;                    ///< next byte to hand out
    };
} // namespace mark4
