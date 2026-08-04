#include "platform_sim/telemetry_sender_sim.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol/telemetry.hpp"

namespace mark4
{
    namespace
    {
        /// @brief Reports a failed system call and the reason behind it.
        /// @param what name of the system call that failed
        void logErrno(const char *what)
        {
            static_cast<void>(std::fprintf(
                stderr, "TelemetrySenderSim: %s failed: %s\n", what, std::strerror(errno)));
        }
    } // namespace

    TelemetrySenderSim::~TelemetrySenderSim()
    {
        if (m_socketFd >= 0)
        {
            static_cast<void>(::close(m_socketFd));
            m_socketFd = -1;
        }
    }

    bool TelemetrySenderSim::open()
    {
        if (m_socketFd >= 0)
        {
            static_cast<void>(std::fprintf(stderr, "TelemetrySenderSim: already open\n"));
            return false;
        }

        m_socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_socketFd < 0)
        {
            logErrno("socket");
            return false;
        }

        const int enable = 1;
        if (::setsockopt(m_socketFd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
        {
            logErrno("setsockopt(SO_BROADCAST)");
            static_cast<void>(::close(m_socketFd));
            m_socketFd = -1;
            return false;
        }
        return true;
    }

    void TelemetrySenderSim::send(const std::uint8_t *data, std::size_t size)
    {
        if (m_socketFd < 0 || data == nullptr)
        {
            return;
        }

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(mark4::TELEMETRY_PORT);
        target.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        const ssize_t sent = ::sendto(
            m_socketFd, data, size, 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
        if (sent < 0)
        {
            if (!m_sendFailureLogged)
            {
                logErrno("sendto");
                m_sendFailureLogged = true;
            }
            return;
        }

        ++m_packetCount;
        m_byteCount += static_cast<std::size_t>(sent);
    }
} // namespace mark4
