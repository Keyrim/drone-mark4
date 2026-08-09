#include "platform_sim/udp_link.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t MS_PER_S = 1000U;
        constexpr std::uint32_t US_PER_MS = 1000U;

        /// @brief Reports a failed system call and the reason behind it.
        /// @param what name of the system call that failed
        void logErrno(const char *what)
        {
            static_cast<void>(
                std::fprintf(stderr, "UdpLink: %s failed: %s\n", what, std::strerror(errno)));
        }
    } // namespace

    UdpLink::~UdpLink()
    {
        closeSocket();
    }

    bool UdpLink::open(std::uint16_t port, std::uint32_t receiveTimeoutMs)
    {
        if (m_socketFd >= 0)
        {
            static_cast<void>(std::fprintf(
                stderr, "UdpLink: already open on port %u\n", static_cast<unsigned>(m_boundPort)));
            return false;
        }

        m_socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_socketFd < 0)
        {
            logErrno("socket");
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(m_socketFd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            logErrno("bind");
            closeSocket();
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(receiveTimeoutMs / MS_PER_S);
        timeout.tv_usec = static_cast<suseconds_t>(receiveTimeoutMs % MS_PER_S) * US_PER_MS;
        if (::setsockopt(m_socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        {
            logErrno("setsockopt(SO_RCVTIMEO)");
            closeSocket();
            return false;
        }

        // Port 0 means "any free port": ask the kernel which one it picked.
        sockaddr_in bound{};
        socklen_t boundSize = sizeof(bound);
        if (::getsockname(m_socketFd, reinterpret_cast<sockaddr *>(&bound), &boundSize) < 0)
        {
            logErrno("getsockname");
            closeSocket();
            return false;
        }
        m_boundPort = ntohs(bound.sin_port);
        return true;
    }

    std::size_t UdpLink::receive(std::uint8_t *bufferOut, std::size_t capacity)
    {
        if (m_socketFd < 0 || bufferOut == nullptr || capacity == 0U)
        {
            return 0U;
        }

        sockaddr_in sender{};
        socklen_t senderSize = sizeof(sender);
        const ssize_t received = ::recvfrom(
            m_socketFd, bufferOut, capacity, 0, reinterpret_cast<sockaddr *>(&sender), &senderSize);
        if (received <= 0)
        {
            // EAGAIN is the receive timeout, EINTR a signal: both are normal.
            if (received < 0 && errno != EAGAIN && errno != EINTR)
            {
                logErrno("recvfrom");
            }
            return 0U;
        }

        m_pendingSender = sender;
        m_hasPendingSender = true;
        return static_cast<std::size_t>(received);
    }

    void UdpLink::acceptLastSender()
    {
        if (m_hasPendingSender)
        {
            m_replyTarget = m_pendingSender;
            m_hasReplyTarget = true;
        }
    }

    bool UdpLink::replyToLastSender(const std::uint8_t *data, std::size_t size)
    {
        if (m_socketFd < 0 || !m_hasReplyTarget || data == nullptr)
        {
            return false;
        }

        const ssize_t sent = ::sendto(m_socketFd,
                                      data,
                                      size,
                                      0,
                                      reinterpret_cast<const sockaddr *>(&m_replyTarget),
                                      sizeof(m_replyTarget));
        if (sent < 0)
        {
            logErrno("sendto");
            return false;
        }
        return static_cast<std::size_t>(sent) == size;
    }

    void UdpLink::closeSocket()
    {
        if (m_socketFd >= 0)
        {
            static_cast<void>(::close(m_socketFd));
            m_socketFd = -1;
        }
    }
} // namespace mark4
