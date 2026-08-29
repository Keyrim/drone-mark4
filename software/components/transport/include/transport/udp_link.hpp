#pragma once

/// @file
/// @brief Transport link over IPv4 UDP, POSIX sockets. Two sockets: one
///        shared discovery port every node binds (SO_REUSEADDR +
///        SO_REUSEPORT) and only ever receives broadcasts on, and one
///        ephemeral data socket every frame leaves from, so the source
///        port of any datagram is the node's unicast address.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "transport/link.hpp"

namespace mark4
{
    /// The one UDP port every node of a deployment must agree on: broadcasts
    /// land here. A batch campaign isolates itself by picking another one.
    inline constexpr std::uint16_t DISCOVERY_PORT = 47820U;

    class UdpLink final : public AbsLink
    {
      public:
        /// @param discoveryPort shared broadcast port of this deployment
        explicit UdpLink(std::uint16_t discoveryPort = DISCOVERY_PORT)
            : m_discoveryPort(discoveryPort)
        {
        }

        ~UdpLink() override;

        UdpLink(const UdpLink &) = delete;
        UdpLink &operator=(const UdpLink &) = delete;
        UdpLink(UdpLink &&) = delete;
        UdpLink &operator=(UdpLink &&) = delete;

        /// @brief Opens and binds both sockets. Logs the first failure.
        /// @return true when frames can flow
        bool init();

        bool send(const std::uint8_t *data, std::size_t size, const LinkAddress &address) override;

        /// @brief Sends to 255.255.255.255:discoveryPort, and to the loopback
        ///        broadcast address instead when the host has no route for
        ///        the former (an isolated container). Linux delivers a
        ///        broadcast to every local socket bound to the port, this
        ///        node's own discovery socket included.
        bool broadcast(const std::uint8_t *data, std::size_t size) override;

        /// @brief Takes one pending datagram, unicast first. A broadcast
        ///        this very socket sent comes back from the kernel like any
        ///        other (own data port, own address) and is dropped here: it
        ///        carries nothing, and when the node relays it would count
        ///        as a duplicate of the frame it just forwarded.
        std::size_t receive(std::uint8_t *bufferOut,
                            std::size_t capacity,
                            LinkAddress &fromOut) override;

        /// @return descriptor of the discovery socket, -1 before init(); for
        ///         a caller that poll()s instead of spinning
        [[nodiscard]] int discoveryFd() const
        {
            return m_discoveryFd;
        }

        /// @return descriptor of the data socket, -1 before init()
        [[nodiscard]] int dataFd() const
        {
            return m_dataFd;
        }

        /// @return port the data socket is bound to, 0 before init()
        [[nodiscard]] std::uint16_t dataPort() const
        {
            return m_dataPort;
        }

        /// @return shared discovery port
        [[nodiscard]] std::uint16_t discoveryPort() const
        {
            return m_discoveryPort;
        }

      private:
        /// @brief Sends one datagram from the data socket.
        /// @param data bytes
        /// @param size byte count
        /// @param host IPv4 destination, host byte order
        /// @param port destination port
        /// @return true when the whole datagram left
        [[nodiscard]] bool sendTo(const std::uint8_t *data,
                                  std::size_t size,
                                  std::uint32_t host,
                                  std::uint16_t port) const;

        /// @brief Non-blocking read of one socket.
        /// @param fd socket to read
        /// @param[out] bufferOut receives the datagram
        /// @param capacity size of bufferOut
        /// @param[out] fromOut sender
        /// @return datagram size, 0 when nothing pending or oversized
        static std::size_t ReadOne(int fd,
                                   std::uint8_t *bufferOut,
                                   std::size_t capacity,
                                   LinkAddress &fromOut);

        /// @param from sender of a datagram read on the discovery socket
        /// @return true when it is this node's own data socket
        [[nodiscard]] bool isOwnEcho(const LinkAddress &from) const;

        /// @brief Closes whatever is open.
        void closeSockets();

        std::uint16_t m_discoveryPort; ///< shared broadcast port
        int m_discoveryFd = -1;        ///< bound to m_discoveryPort, -1 when closed
        int m_dataFd = -1;             ///< bound to m_dataPort, -1 when closed
        std::uint16_t m_dataPort = 0U; ///< ephemeral port the kernel picked
        bool m_loopbackOnly = false;   ///< 255.255.255.255 failed once: use 127.255.255.255
        std::vector<std::uint32_t> m_localHosts; ///< this host's IPv4 addresses at init(),
                                                 ///< host byte order, echo detection
    };
} // namespace mark4
