#pragma once

/// @file
/// @brief The UDP side of the hub: one listening socket per port it watches,
///        one sending socket for everything it emits. Ports come and go as
///        processes appear and disappear, so subscribing is a runtime
///        operation, not a startup one.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <vector>

namespace mark4
{
    /// Set of UDP sockets the hub listens on, plus the one it sends from.
    /// Every listener sets SO_REUSEADDR: the telemetry ports are shared
    /// broadcasts and other consumers must keep working next to the hub.
    class UdpTransport
    {
      public:
        /// Largest datagram the hub accepts. Every protocol/ packet is far
        /// smaller; anything bigger is not ours.
        static constexpr std::size_t MAX_DATAGRAM = 2048U;

        /// Datagrams read from one socket per drain(), so a flooded port
        /// cannot starve the others or the housekeeping.
        static constexpr unsigned MAX_DRAIN_PER_SOCKET = 256U;

        /// Called once per received datagram, with the local port it landed on.
        using Handler = std::function<void(std::uint16_t, const std::uint8_t *, std::size_t)>;

        UdpTransport() = default;
        UdpTransport(const UdpTransport &) = delete;
        UdpTransport &operator=(const UdpTransport &) = delete;
        UdpTransport(UdpTransport &&) = delete;
        UdpTransport &operator=(UdpTransport &&) = delete;
        ~UdpTransport();

        /// @brief Opens the sending socket and the announce listener.
        /// @param announcePort port every process broadcasts its announce to
        /// @return true when both sockets are ready
        bool init(std::uint16_t announcePort);

        /// @brief Starts listening on one more port. Idempotent: subscribing
        ///        twice to the same port keeps one socket.
        /// @param port port to listen on
        /// @return true when the port is listened on
        bool subscribe(std::uint16_t port);

        /// @brief Stops listening on one port. Does nothing when the port was
        ///        not subscribed.
        /// @param port port to drop
        void unsubscribe(std::uint16_t port);

        /// @return true when the port has a listening socket
        [[nodiscard]] bool subscribed(std::uint16_t port) const;

        /// @brief Appends one poll entry per listening socket.
        /// @param fds poll set being built
        void appendPollFds(std::vector<pollfd> &fds) const;

        /// @brief Reads everything pending on every listening socket.
        /// @param handler called once per datagram
        void drain(const Handler &handler);

        /// @brief Sends one datagram to one host. Const because what changes
        ///        is the state of the socket in the kernel, not this object.
        /// @param data packet bytes
        /// @param size packet size
        /// @param host destination host, dotted quad
        /// @param port destination port
        /// @return true when the datagram was handed to the kernel
        bool sendTo(const std::uint8_t *data,
                    std::size_t size,
                    const char *host,
                    std::uint16_t port) const;

        /// @brief Sends one datagram to the local broadcast address.
        /// @param data packet bytes
        /// @param size packet size
        /// @param port destination port
        /// @return true when the datagram was handed to the kernel
        bool broadcast(const std::uint8_t *data, std::size_t size, std::uint16_t port) const;

      private:
        /// One listening socket and the port it is bound to.
        struct Listener
        {
            std::uint16_t port = 0U; ///< bound port
            int fd = -1;             ///< socket descriptor
        };

        std::vector<Listener> m_listeners; ///< one entry per subscribed port
        int m_sendFd = -1;                 ///< socket every outgoing datagram leaves by
    };
} // namespace mark4
