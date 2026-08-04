#pragma once

/// @file
/// @brief RAII wrapper around a bound POSIX UDP socket.

#include <cstddef>
#include <cstdint>

#include <netinet/in.h>

namespace mark4
{
    /// Owns one UDP socket bound to a local port: blocking receive with a
    /// timeout, and a reply channel back to whoever sent the last datagram.
    /// The socket is closed by the destructor; the object is neither copyable
    /// nor movable so that the file descriptor has a single owner.
    class UdpLink
    {
      public:
        UdpLink() = default;
        ~UdpLink();

        UdpLink(const UdpLink &) = delete;
        UdpLink &operator=(const UdpLink &) = delete;
        UdpLink(UdpLink &&) = delete;
        UdpLink &operator=(UdpLink &&) = delete;

        /// @brief Creates the socket and binds it to every local interface.
        /// @param port local UDP port, 0 to let the system pick a free one
        /// @param receiveTimeoutMs delay after which receive() gives up
        /// @return true when the socket is bound and ready
        bool open(std::uint16_t port, std::uint32_t receiveTimeoutMs);

        /// @return local port the socket is bound to, 0 when it is not open
        [[nodiscard]] std::uint16_t boundPort() const
        {
            return m_boundPort;
        }

        /// @brief Blocks until one datagram arrives or the receive timeout
        ///        expires. On success the sender address is recorded, so that
        ///        replyToLastSender() knows where to answer.
        /// @param[out] bufferOut receives the datagram bytes
        /// @param capacity size of bufferOut; a longer datagram is truncated
        /// @return number of bytes received, 0 on timeout or error
        std::size_t receive(std::uint8_t *bufferOut, std::size_t capacity);

        /// @brief Sends one datagram back to the sender of the last datagram
        ///        received.
        /// @param data bytes to send
        /// @param size number of bytes to send
        /// @return true when the whole datagram was handed to the stack
        bool replyToLastSender(const std::uint8_t *data, std::size_t size);

      private:
        /// @brief Closes the socket if it is open and marks it closed.
        void closeSocket();

        int m_socketFd = -1;            ///< -1 when closed
        std::uint16_t m_boundPort = 0U; ///< local port, resolved after bind
        sockaddr_in m_lastSender{};     ///< source of the last datagram received
        bool m_hasLastSender = false;   ///< true once a datagram was received
    };
} // namespace mark4
