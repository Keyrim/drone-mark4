#pragma once

/// @file
/// @brief A stand-in for the ESP32 bridge, on the loopback: two UDP sockets,
///        each learning its peer from the first datagram it receives and
///        forwarding every datagram unchanged to the other side's peer. The
///        real bridge does exactly this with a UART on one side; here both
///        sides are UDP so a test drives both ends.

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mark4
{
    class FakeBridge
    {
      public:
        /// Largest datagram forwarded, like the real bridge's UART gather.
        static constexpr std::size_t MAX_DATAGRAM = 1024U;

        FakeBridge()
        {
            m_ok = OpenSide(m_ground) && OpenSide(m_board);
        }
        FakeBridge(const FakeBridge &) = delete;
        FakeBridge &operator=(const FakeBridge &) = delete;
        FakeBridge(FakeBridge &&) = delete;
        FakeBridge &operator=(FakeBridge &&) = delete;
        ~FakeBridge()
        {
            CloseSide(m_ground);
            CloseSide(m_board);
        }

        /// @return true when both sockets are bound
        [[nodiscard]] bool ok() const
        {
            return m_ok;
        }

        /// @return port the ground tool (the hub) sends to
        [[nodiscard]] std::uint16_t groundPort() const
        {
            return m_ground.port;
        }

        /// @return port the board side sends to (the UART, in reality)
        [[nodiscard]] std::uint16_t boardPort() const
        {
            return m_board.port;
        }

        /// @brief Moves every pending datagram across, without blocking. A
        ///        datagram for a side whose peer is unknown yet is dropped,
        ///        as the real bridge drops UART bytes before any hello.
        void pump()
        {
            pumpSide(m_ground, m_board);
            pumpSide(m_board, m_ground);
        }

        /// @return datagrams forwarded so far
        [[nodiscard]] std::uint32_t forwarded() const
        {
            return m_forwarded;
        }

      private:
        struct Side
        {
            int fd = -1;             ///< bound socket
            std::uint16_t port = 0U; ///< its port on the loopback
            sockaddr_in peer{};      ///< who last spoke to it
            bool peerKnown = false;  ///< true once someone did
        };

        static bool OpenSide(Side &side)
        {
            side.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            if (side.fd < 0)
            {
                return false;
            }
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            local.sin_port = 0U;
            if (::bind(side.fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
            {
                return false;
            }
            socklen_t length = sizeof(local);
            if (::getsockname(side.fd, reinterpret_cast<sockaddr *>(&local), &length) != 0)
            {
                return false;
            }
            side.port = ntohs(local.sin_port);
            return true;
        }

        static void CloseSide(Side &side)
        {
            if (side.fd >= 0)
            {
                static_cast<void>(::close(side.fd));
                side.fd = -1;
            }
        }

        void pumpSide(Side &from, Side &to)
        {
            if (from.fd < 0 || to.fd < 0)
            {
                return;
            }
            std::array<std::uint8_t, MAX_DATAGRAM> scratch{};
            for (;;)
            {
                sockaddr_in sender{};
                socklen_t length = sizeof(sender);
                const ssize_t size = ::recvfrom(from.fd,
                                                scratch.data(),
                                                scratch.size(),
                                                0,
                                                reinterpret_cast<sockaddr *>(&sender),
                                                &length);
                if (size <= 0)
                {
                    return;
                }
                from.peer = sender;
                from.peerKnown = true;
                if (!to.peerKnown)
                {
                    continue;
                }
                if (::sendto(to.fd,
                             scratch.data(),
                             static_cast<std::size_t>(size),
                             0,
                             reinterpret_cast<const sockaddr *>(&to.peer),
                             sizeof(to.peer)) == size)
                {
                    ++m_forwarded;
                }
            }
        }

        Side m_ground;                  ///< the hub's side
        Side m_board;                   ///< the board's side
        bool m_ok = false;              ///< both sockets bound
        std::uint32_t m_forwarded = 0U; ///< datagrams moved across
    };
} // namespace mark4
