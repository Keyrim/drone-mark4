#include "transport/udp_link.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mark4
{
    namespace
    {
        /// 255.255.255.255, host byte order.
        constexpr std::uint32_t GLOBAL_BROADCAST = INADDR_BROADCAST;

        /// 127.255.255.255, host byte order: the broadcast address of the
        /// loopback network, which Linux delivers to every local listener.
        constexpr std::uint32_t LOOPBACK_BROADCAST = 0x7FFFFFFFU;

        /// @brief Reports a failed system call and the reason behind it.
        /// @param what name of the system call that failed
        void logErrno(const char *what)
        {
            static_cast<void>(
                std::fprintf(stderr, "UdpLink: %s failed: %s\n", what, std::strerror(errno)));
        }

        /// @brief Opens one UDP socket bound to INADDR_ANY.
        /// @param port port to bind, 0 for an ephemeral one
        /// @param shared true to allow other sockets on the same port
        /// @return descriptor, -1 on failure (logged)
        int openBound(std::uint16_t port, bool shared)
        {
            const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0)
            {
                logErrno("socket");
                return -1;
            }
            const int enable = 1;
            if (shared)
            {
                static_cast<void>(
                    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)));
                static_cast<void>(
                    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)));
            }
            if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
            {
                logErrno("setsockopt(SO_BROADCAST)");
                static_cast<void>(::close(fd));
                return -1;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
            {
                logErrno("bind");
                static_cast<void>(::close(fd));
                return -1;
            }
            return fd;
        }
    } // namespace

    UdpLink::~UdpLink()
    {
        closeSockets();
    }

    bool UdpLink::init()
    {
        if (m_discoveryFd >= 0 || m_dataFd >= 0)
        {
            static_cast<void>(std::fprintf(stderr, "UdpLink: already open\n"));
            return false;
        }
        m_discoveryFd = openBound(m_discoveryPort, true);
        if (m_discoveryFd < 0)
        {
            return false;
        }
        m_dataFd = openBound(0U, false);
        if (m_dataFd < 0)
        {
            closeSockets();
            return false;
        }
        sockaddr_in bound{};
        socklen_t boundSize = sizeof(bound);
        if (::getsockname(m_dataFd, reinterpret_cast<sockaddr *>(&bound), &boundSize) < 0)
        {
            logErrno("getsockname");
            closeSockets();
            return false;
        }
        m_dataPort = ntohs(bound.sin_port);
        return true;
    }

    bool UdpLink::send(const std::uint8_t *data, std::size_t size, const LinkAddress &address)
    {
        return sendTo(data, size, address.host, address.port);
    }

    bool UdpLink::broadcast(const std::uint8_t *data, std::size_t size)
    {
        if (!m_loopbackOnly)
        {
            if (sendTo(data, size, GLOBAL_BROADCAST, m_discoveryPort))
            {
                return true;
            }
            m_loopbackOnly = true;
            static_cast<void>(std::fprintf(stderr,
                                           "UdpLink: no route for 255.255.255.255 (%s): "
                                           "broadcasting on the loopback only\n",
                                           std::strerror(errno)));
        }
        return sendTo(data, size, LOOPBACK_BROADCAST, m_discoveryPort);
    }

    std::size_t UdpLink::receive(std::uint8_t *bufferOut,
                                 std::size_t capacity,
                                 LinkAddress &fromOut)
    {
        if (m_dataFd < 0)
        {
            return 0U;
        }
        const std::size_t unicast = ReadOne(m_dataFd, bufferOut, capacity, fromOut);
        if (unicast > 0U)
        {
            return unicast;
        }
        return ReadOne(m_discoveryFd, bufferOut, capacity, fromOut);
    }

    bool UdpLink::sendTo(const std::uint8_t *data,
                         std::size_t size,
                         std::uint32_t host,
                         std::uint16_t port) const
    {
        if (m_dataFd < 0 || data == nullptr || port == 0U)
        {
            return false;
        }
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        target.sin_addr.s_addr = htonl(host);
        const ssize_t sent = ::sendto(
            m_dataFd, data, size, 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
        return sent == static_cast<ssize_t>(size);
    }

    std::size_t UdpLink::ReadOne(int fd,
                                 std::uint8_t *bufferOut,
                                 std::size_t capacity,
                                 LinkAddress &fromOut)
    {
        for (;;)
        {
            sockaddr_in from{};
            socklen_t fromSize = sizeof(from);
            const ssize_t received = ::recvfrom(fd,
                                                bufferOut,
                                                capacity,
                                                MSG_DONTWAIT | MSG_TRUNC,
                                                reinterpret_cast<sockaddr *>(&from),
                                                &fromSize);
            if (received <= 0)
            {
                if (received < 0 && errno != EAGAIN && errno != EINTR)
                {
                    logErrno("recvfrom");
                }
                return 0U;
            }
            if (static_cast<std::size_t>(received) > capacity)
            {
                continue; // oversized: not one of ours, take the next
            }
            fromOut.host = ntohl(from.sin_addr.s_addr);
            fromOut.port = ntohs(from.sin_port);
            return static_cast<std::size_t>(received);
        }
    }

    void UdpLink::closeSockets()
    {
        for (int *fd : {&m_discoveryFd, &m_dataFd})
        {
            if (*fd >= 0)
            {
                static_cast<void>(::close(*fd));
                *fd = -1;
            }
        }
        m_dataPort = 0U;
    }
} // namespace mark4
