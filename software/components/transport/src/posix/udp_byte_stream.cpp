#include "transport/udp_byte_stream.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mark4
{
    UdpByteStream::~UdpByteStream()
    {
        close();
    }

    bool UdpByteStream::open(const char *host, std::uint16_t port)
    {
        close();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (host == nullptr || port == 0U || ::inet_pton(AF_INET, host, &address.sin_addr) != 1)
        {
            return false;
        }
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
        {
            return false;
        }
        // Connecting a datagram socket picks the route, fixes the source
        // address the peer learns, and filters what comes back to that peer.
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            static_cast<void>(::close(fd));
            return false;
        }
        m_fd = fd;
        m_pendingSize = 0U;
        m_pendingIndex = 0U;
        return true;
    }

    void UdpByteStream::close()
    {
        if (m_fd >= 0)
        {
            static_cast<void>(::close(m_fd));
            m_fd = -1;
        }
    }

    std::size_t UdpByteStream::read(std::uint8_t *bufferOut, std::size_t capacity)
    {
        if (m_pendingIndex >= m_pendingSize)
        {
            if (m_fd < 0)
            {
                return 0U;
            }
            // A datagram is taken whole, or its tail would be lost: the
            // caller's buffer may be smaller than one, so it drains it
            // over several reads.
            const ssize_t received = ::recv(m_fd, m_pending.data(), m_pending.size(), 0);
            if (received <= 0)
            {
                // ECONNREFUSED is the peer's ICMP answer to a datagram sent
                // while it was down: nothing to read, the socket stays usable.
                return 0U;
            }
            m_pendingSize = static_cast<std::size_t>(received);
            m_pendingIndex = 0U;
        }
        const std::size_t count = std::min(capacity, m_pendingSize - m_pendingIndex);
        std::memcpy(bufferOut, m_pending.data() + m_pendingIndex, count);
        m_pendingIndex += count;
        return count;
    }

    bool UdpByteStream::write(const std::uint8_t *data, std::size_t size)
    {
        if (m_fd < 0)
        {
            return false;
        }
        return ::send(m_fd, data, size, 0) == static_cast<ssize_t>(size);
    }
} // namespace mark4
