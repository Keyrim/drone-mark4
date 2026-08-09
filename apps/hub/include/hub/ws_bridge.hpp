#pragma once

/// @file
/// @brief The websocket endpoint of the hub: the one place a browser or a
///        script can watch the decoded streams and send commands back,
///        without ever touching a UDP socket or a packed struct.
///
///        Threading contract: the library runs one thread per client and
///        calls back from those threads. Inbound messages are only queued
///        there; everything else (parsing, routing, broadcasting) happens in
///        the poll loop, so the rest of the hub stays single-threaded.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ix
{
    class WebSocketServer;
} // namespace ix

namespace mark4
{
    /// Websocket server the hub publishes to and takes commands from.
    class WsBridge
    {
      public:
        /// Inbound messages kept while the poll loop is busy. Past this, the
        /// oldest are dropped: a client flooding the endpoint must not grow
        /// the queue without bound.
        static constexpr std::size_t MAX_INBOUND = 1024U;

        WsBridge();
        WsBridge(const WsBridge &) = delete;
        WsBridge &operator=(const WsBridge &) = delete;
        WsBridge(WsBridge &&) = delete;
        WsBridge &operator=(WsBridge &&) = delete;
        ~WsBridge();

        /// @brief Binds and starts serving.
        /// @param port TCP port to listen on
        /// @return true when the endpoint accepts connections
        bool start(std::uint16_t port);

        /// @brief Stops serving and closes every connection.
        void stop();

        /// @brief Sends one text message to every connected client. Called
        ///        from the poll loop only.
        /// @param text message to send
        void broadcastText(const std::string &text);

        /// @brief Takes everything clients have sent since the last call.
        /// @return the messages, oldest first
        std::vector<std::string> drainInbound();

        /// @return number of connected clients
        [[nodiscard]] std::size_t clientCount() const
        {
            return m_clients.load();
        }

        /// @brief Reports whether a client connected since the last call, so
        ///        the poll loop knows it owes the world a fresh snapshot of
        ///        the discovery table and the counters.
        /// @return true when at least one client connected since last asked
        bool takeConnectedFlag()
        {
            return m_connected.exchange(false);
        }

      private:
        std::unique_ptr<ix::WebSocketServer> m_server; ///< the library server, null until start()
        std::mutex m_inboundMutex;                     ///< guards the inbound queue
        std::vector<std::string> m_inbound;            ///< messages waiting for the poll loop
        std::atomic_size_t m_clients{0U};              ///< connected clients
        std::atomic_bool m_connected{false};           ///< a client connected since last asked
    };
} // namespace mark4
