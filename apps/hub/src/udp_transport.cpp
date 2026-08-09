/// @file
/// @brief UDP transport implementation. POSIX sockets, non-blocking, drained
///        from the single poll loop of the hub.

#include "hub/udp_transport.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mark4
{
    namespace
    {
        /// @brief Fills a v4 address for the given host and port.
        /// @param host dotted quad, nullptr for "any"
        /// @param port port number
        /// @param addressOut receives the address
        /// @return true when the host text parsed
        bool fillAddress(const char *host, std::uint16_t port, sockaddr_in &addressOut)
        {
            addressOut = {};
            addressOut.sin_family = AF_INET;
            addressOut.sin_port = htons(port);
            if (host == nullptr)
            {
                addressOut.sin_addr.s_addr = htonl(INADDR_ANY);
                return true;
            }
            return inet_pton(AF_INET, host, &addressOut.sin_addr) == 1;
        }
    } // namespace

    UdpTransport::~UdpTransport()
    {
        for (const Listener &listener : m_listeners)
        {
            static_cast<void>(::close(listener.fd));
        }
        if (m_sendFd >= 0)
        {
            static_cast<void>(::close(m_sendFd));
        }
    }

    bool UdpTransport::init(std::uint16_t announcePort)
    {
        m_sendFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sendFd < 0)
        {
            static_cast<void>(std::fprintf(
                stderr, "hub: cannot open the sending socket: %s\n", std::strerror(errno)));
            return false;
        }
        const int enabled = 1;
        if (::setsockopt(m_sendFd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0)
        {
            static_cast<void>(
                std::fprintf(stderr, "hub: cannot enable broadcast: %s\n", std::strerror(errno)));
            return false;
        }
        return subscribe(announcePort);
    }

    bool UdpTransport::subscribed(std::uint16_t port) const
    {
        return std::any_of(m_listeners.begin(),
                           m_listeners.end(),
                           [port](const Listener &listener) { return listener.port == port; });
    }

    bool UdpTransport::subscribe(std::uint16_t port)
    {
        if (subscribed(port))
        {
            return true;
        }

        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            static_cast<void>(std::fprintf(stderr,
                                           "hub: cannot open a listener for udp/%u: %s\n",
                                           static_cast<unsigned>(port),
                                           std::strerror(errno)));
            return false;
        }
        // Every stream port of the project is a shared broadcast: the hub is
        // one consumer among others and must never take a port hostage.
        const int enabled = 1;
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)));

        sockaddr_in address{};
        static_cast<void>(fillAddress(nullptr, port, address));
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            static_cast<void>(std::fprintf(stderr,
                                           "hub: cannot bind udp/%u: %s\n",
                                           static_cast<unsigned>(port),
                                           std::strerror(errno)));
            static_cast<void>(::close(fd));
            return false;
        }
        m_listeners.push_back(Listener{port, fd});
        return true;
    }

    void UdpTransport::unsubscribe(std::uint16_t port)
    {
        const auto found =
            std::find_if(m_listeners.begin(), m_listeners.end(), [port](const Listener &listener) {
                return listener.port == port;
            });
        if (found == m_listeners.end())
        {
            return;
        }
        static_cast<void>(::close(found->fd));
        static_cast<void>(m_listeners.erase(found));
    }

    void UdpTransport::appendPollFds(std::vector<pollfd> &fds) const
    {
        for (const Listener &listener : m_listeners)
        {
            pollfd entry{};
            entry.fd = listener.fd;
            entry.events = POLLIN;
            fds.push_back(entry);
        }
    }

    void UdpTransport::drain(const Handler &handler)
    {
        std::array<std::uint8_t, MAX_DATAGRAM> datagram{};
        // The listener set can only change from the handler (a discovery
        // event subscribes or unsubscribes a port), so it is indexed rather
        // than iterated: a reallocation must not invalidate the walk.
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t index = 0U; index < m_listeners.size(); ++index)
        {
            const int fd = m_listeners[index].fd;
            const std::uint16_t port = m_listeners[index].port;
            for (unsigned read = 0U; read < MAX_DRAIN_PER_SOCKET; ++read)
            {
                const ssize_t size = ::recv(fd, datagram.data(), datagram.size(), MSG_DONTWAIT);
                if (size <= 0)
                {
                    break;
                }
                handler(port, datagram.data(), static_cast<std::size_t>(size));
            }
        }
    }

    bool UdpTransport::sendTo(const std::uint8_t *data,
                              std::size_t size,
                              const char *host,
                              std::uint16_t port) const
    {
        sockaddr_in address{};
        if (m_sendFd < 0 || !fillAddress(host, port, address))
        {
            return false;
        }
        const ssize_t sent = ::sendto(
            m_sendFd, data, size, 0, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        return sent == static_cast<ssize_t>(size);
    }

    bool UdpTransport::broadcast(const std::uint8_t *data,
                                 std::size_t size,
                                 std::uint16_t port) const
    {
        return sendTo(data, size, "255.255.255.255", port);
    }
} // namespace mark4
