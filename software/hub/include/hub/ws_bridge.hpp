#pragma once

/// @file
/// @brief The endpoint of the gateway: where a browser or a script reads
///        GatewayMessage binaries and sends them back. One TCP port carries
///        both the websocket and the static pages, dispatched on the
///        Upgrade header, so a page reaches the hub at the host it was
///        loaded from and never learns a port.
///
///        Threading contract: the library runs one thread per connection and
///        calls back from those threads. Inbound websocket messages are only
///        queued there; everything else (parsing, routing, broadcasting)
///        happens in the poll loop, so the rest of the hub stays
///        single-threaded. HTTP requests are answered on the connection
///        thread from the filesystem alone, touching no hub state.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hub/http_api.hpp"

namespace ix
{
    class HttpServer;
    class WebSocket;
} // namespace ix

namespace mark4
{
    /// One inbound websocket message, tagged with the connection it came
    /// from so the poll loop can tell two clients apart (the RC warning
    /// counts pilots, not tabs).
    struct InboundMessage
    {
        std::string clientId; ///< library id of the connection
        std::string bytes;    ///< the binary message body
    };

    /// Endpoint the hub publishes to, takes commands from, and serves the
    /// pages of.
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
        /// @param bindAddress address to bind to
        /// @param http what the HTTP side reads from; copied here and read
        ///        by the connection threads afterwards, never written again
        /// @return true when the endpoint accepts connections
        bool start(std::uint16_t port, const std::string &bindAddress, HttpConfig http);

        /// @brief Stops serving and closes every connection.
        void stop();

        /// @brief Sends one binary message to every connected client. Called
        ///        from the poll loop only.
        /// @param bytes message to send
        void broadcastBinary(const std::string &bytes);

        /// @brief Sends one binary message to one client. Called from the
        ///        poll loop only, like the broadcast.
        /// @param clientId library id of the connection
        /// @param bytes message to send
        /// @return true when the client is still connected and took it
        bool sendBinary(const std::string &clientId, const std::string &bytes);

        /// @brief Takes everything clients have sent since the last call.
        /// @return the messages, oldest first
        std::vector<InboundMessage> drainInbound();

        /// @brief Counts the live connections, as the library tracks them.
        ///        Called from the poll loop only.
        /// @return number of connected clients
        [[nodiscard]] std::size_t clientCount() const;

        /// @brief Takes the clients that connected since the last call: each
        ///        of them owes a snapshot of everything the gateway holds,
        ///        and it goes to that client alone rather than to everyone.
        /// @return their connection ids, oldest first
        std::vector<std::string> drainConnected();

        /// @brief Takes the clients whose connection closed since the last
        ///        call, so whatever was kept per client goes with them.
        /// @return their connection ids, oldest first
        std::vector<std::string> drainClosed();

      private:
        std::unique_ptr<ix::HttpServer> m_server; ///< the library server, null until start()
        HttpConfig m_http;                        ///< what the HTTP side reads from
        std::mutex m_inboundMutex;                ///< guards the inbound queue
        std::vector<InboundMessage> m_inbound;    ///< messages waiting for the poll loop
        std::mutex m_clientsMutex;                ///< guards the three members below
        /// The live connections, by library id: what a message to one client
        /// alone is sent through.
        std::map<std::string, ix::WebSocket *> m_clients;
        std::vector<std::string> m_connected; ///< connected since last drained
        std::vector<std::string> m_closed;    ///< closed since last drained
    };
} // namespace mark4
