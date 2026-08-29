#include "transport/udp_link.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// lwIP has no getifaddrs(): the ESP32 relay names its address itself.
#if __has_include(<ifaddrs.h>)
#include <ifaddrs.h>
#define MARK4_HAVE_IFADDRS 1
#else
#define MARK4_HAVE_IFADDRS 0
#endif

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
#if MARK4_HAVE_IFADDRS
        // Every address a broadcast of ours can come back from. Best
        // effort: without the list an echo is merely counted as a duplicate.
        ifaddrs *interfaces = nullptr;
        if (::getifaddrs(&interfaces) == 0)
        {
            for (const ifaddrs *entry = interfaces; entry != nullptr; entry = entry->ifa_next)
            {
                if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET)
                {
                    const auto *address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
                    m_localHosts.push_back(ntohl(address->sin_addr.s_addr));
                }
            }
            ::freeifaddrs(interfaces);
        }
#endif
        return true;
    }

    bool UdpLink::send(const std::uint8_t *data, std::size_t size, const LinkAddress &address)
    {
        return sendTo(data, size, address.host, address.port);
    }

    bool UdpLink::broadcast(const std::uint8_t *data, std::size_t size)
    {
        if (sendTo(data, size, GLOBAL_BROADCAST, m_discoveryPort))
        {
            return true;
        }
        // Tried again on every send: a network that comes back (a WiFi
        // link, a host that was briefly offline) must not leave the node
        // talking to itself for the rest of its run.
        if (!m_loopbackWarned)
        {
            m_loopbackWarned = true;
            static_cast<void>(std::fprintf(stderr,
                                           "UdpLink: no route for 255.255.255.255 (%s): "
                                           "broadcasting on the loopback until one appears\n",
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
        for (;;)
        {
            const std::size_t size = ReadOne(m_discoveryFd, bufferOut, capacity, fromOut);
            if (size == 0U || !isOwnEcho(fromOut))
            {
                return size;
            }
        }
    }

    bool UdpLink::isOwnEcho(const LinkAddress &from) const
    {
        if (from.port != m_dataPort)
        {
            return false;
        }
        return std::ranges::any_of(m_localHosts,
                                   [&from](std::uint32_t host) { return host == from.host; });
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
        m_localHosts.clear();
    }
} // namespace mark4
