/// @file
/// @brief Websocket bridge implementation.

#include "hub/ws_bridge.hpp"

#include <cstdio>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <utility>

namespace mark4
{
    WsBridge::WsBridge() = default;

    WsBridge::~WsBridge()
    {
        stop();
    }

    bool WsBridge::start(std::uint16_t port)
    {
        stop();
        m_server = std::make_unique<ix::WebSocketServer>(static_cast<int>(port));
        m_server->setOnClientMessageCallback(
            [this](const std::shared_ptr<ix::ConnectionState> &state,
                   ix::WebSocket &socket,
                   const ix::WebSocketMessagePtr &message) {
                static_cast<void>(state);
                static_cast<void>(socket);
                switch (message->type)
                {
                    case ix::WebSocketMessageType::Open:
                        ++m_clients;
                        m_connected.store(true);
                        break;
                    case ix::WebSocketMessageType::Close:
                    case ix::WebSocketMessageType::Error:
                        if (m_clients.load() > 0U)
                        {
                            --m_clients;
                        }
                        break;
                    case ix::WebSocketMessageType::Message: {
                        const std::lock_guard<std::mutex> guard(m_inboundMutex);
                        if (m_inbound.size() < MAX_INBOUND)
                        {
                            m_inbound.push_back(message->str);
                        }
                        break;
                    }
                    default:
                        break;
                }
            });

        const std::pair<bool, std::string> listening = m_server->listen();
        if (!listening.first)
        {
            static_cast<void>(std::fprintf(stderr,
                                           "hub: cannot serve the websocket on tcp/%u: %s\n",
                                           static_cast<unsigned>(port),
                                           listening.second.c_str()));
            m_server.reset();
            return false;
        }
        m_server->start();
        return true;
    }

    void WsBridge::stop()
    {
        if (m_server)
        {
            m_server->stop();
            m_server.reset();
        }
        m_clients.store(0U);
    }

    void WsBridge::broadcastText(const std::string &text)
    {
        if (!m_server)
        {
            return;
        }
        for (const std::shared_ptr<ix::WebSocket> &client : m_server->getClients())
        {
            static_cast<void>(client->sendText(text));
        }
    }

    std::vector<std::string> WsBridge::drainInbound()
    {
        std::vector<std::string> taken;
        const std::lock_guard<std::mutex> guard(m_inboundMutex);
        taken.swap(m_inbound);
        return taken;
    }
} // namespace mark4
