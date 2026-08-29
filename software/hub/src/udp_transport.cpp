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
        /// @brief Fills a v4 wildcard address for the given port.
        /// @param port port number
        /// @param addressOut receives the address
        void fillAddress(std::uint16_t port, sockaddr_in &addressOut)
        {
            addressOut = {};
            addressOut.sin_family = AF_INET;
            addressOut.sin_port = htons(port);
            addressOut.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    } // namespace

    UdpTransport::~UdpTransport()
    {
        for (const Listener &listener : m_listeners)
        {
            static_cast<void>(::close(listener.fd));
        }
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
        fillAddress(port, address);
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
                sockaddr_in from{};
                socklen_t fromSize = sizeof(from);
                const ssize_t size = ::recvfrom(fd,
                                                datagram.data(),
                                                datagram.size(),
                                                MSG_DONTWAIT,
                                                reinterpret_cast<sockaddr *>(&from),
                                                &fromSize);
                if (size <= 0)
                {
                    break;
                }
                std::array<char, INET_ADDRSTRLEN> address{};
                Source source;
                source.address =
                    ::inet_ntop(AF_INET, &from.sin_addr, address.data(), address.size());
                source.port = ntohs(from.sin_port);
                handler(port, source, datagram.data(), static_cast<std::size_t>(size));
            }
        }
    }

} // namespace mark4
