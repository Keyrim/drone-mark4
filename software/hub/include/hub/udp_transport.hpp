#pragma once

/// @file
/// @brief The raw UDP listeners of the hub: the ports it watches outside the
///        transport, the simulator's raw state and the bridge announces.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <vector>

namespace mark4
{
    /// Set of UDP sockets the hub listens on. Every listener sets
    /// SO_REUSEADDR: these ports are shared broadcasts and other consumers
    /// must keep working next to the hub.
    class UdpTransport
    {
      public:
        /// Largest datagram the hub accepts. Every protocol/ packet is far
        /// smaller; anything bigger is not ours.
        static constexpr std::size_t MAX_DATAGRAM = 2048U;

        /// Datagrams read from one socket per drain(), so a flooded port
        /// cannot starve the others or the housekeeping.
        static constexpr unsigned MAX_DRAIN_PER_SOCKET = 256U;

        /// Where one datagram came from. The address is a dotted quad valid
        /// for the duration of the handler call only.
        struct Source
        {
            const char *address = nullptr; ///< sender address, dotted quad
            std::uint16_t port = 0U;       ///< sender port
        };

        /// Called once per received datagram, with the local port it landed
        /// on and the address it came from.
        using Handler =
            std::function<void(std::uint16_t, const Source &, const std::uint8_t *, std::size_t)>;

        UdpTransport() = default;
        UdpTransport(const UdpTransport &) = delete;
        UdpTransport &operator=(const UdpTransport &) = delete;
        UdpTransport(UdpTransport &&) = delete;
        UdpTransport &operator=(UdpTransport &&) = delete;
        ~UdpTransport();

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

      private:
        /// One listening socket and the port it is bound to.
        struct Listener
        {
            std::uint16_t port = 0U; ///< bound port
            int fd = -1;             ///< socket descriptor
        };

        std::vector<Listener> m_listeners; ///< one entry per subscribed port
    };
} // namespace mark4
