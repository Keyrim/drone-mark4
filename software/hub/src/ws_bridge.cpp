/// @file
/// @brief Endpoint implementation.

#include "hub/ws_bridge.hpp"

#include <cstdio>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <memory>
#include <utility>

namespace mark4
{
    namespace
    {
        /// @brief Reason phrase of a status code, for the response line.
        /// @param status status code to name
        /// @return the phrase
        const char *statusPhrase(int status)
        {
            switch (status)
            {
                case HTTP_BAD_REQUEST:
                    return "Bad Request";
                case HTTP_NOT_FOUND:
                    return "Not Found";
                case HTTP_METHOD_NOT_ALLOWED:
                    return "Method Not Allowed";
                default:
                    return "OK";
            }
        }
    } // namespace

    WsBridge::WsBridge() = default;

    WsBridge::~WsBridge()
    {
        stop();
    }

    bool WsBridge::start(std::uint16_t port, const std::string &bindAddress, HttpConfig http)
    {
        stop();
        m_http = std::move(http);
        // ix::HttpServer IS an ix::WebSocketServer: it parses the request
        // line, hands a connection carrying "Upgrade: websocket" to the
        // websocket path untouched, and calls the connection callback for
        // everything else. One port, both protocols, no dispatch of ours.
        m_server = std::make_unique<ix::HttpServer>(static_cast<int>(port), bindAddress);
        m_server->setOnConnectionCallback(
            [this](const ix::HttpRequestPtr &request,
                   const std::shared_ptr<ix::ConnectionState> &state) -> ix::HttpResponsePtr {
                static_cast<void>(state);
                const HttpResult result = routeHttp(m_http, request->method, request->uri);
                ix::WebSocketHttpHeaders headers;
                headers["Content-Type"] = result.contentType;
                // A bench page must never read a stale recording out of a
                // browser cache, and nothing served here is worth keeping.
                headers["Cache-Control"] = "no-store";
                if (!result.attachmentName.empty())
                {
                    headers["Content-Disposition"] =
                        "attachment; filename=\"" + result.attachmentName + "\"";
                }
                return std::make_shared<ix::HttpResponse>(result.status,
                                                          statusPhrase(result.status),
                                                          ix::HttpErrorCode::Ok,
                                                          headers,
                                                          result.body);
            });
        m_server->setOnClientMessageCallback(
            [this](const std::shared_ptr<ix::ConnectionState> &state,
                   ix::WebSocket &socket,
                   const ix::WebSocketMessagePtr &message) {
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
                        if (!message->binary)
                        {
                            // The contract is binary GatewayMessage only; a
                            // text frame is not for this endpoint.
                            break;
                        }
                        const std::lock_guard<std::mutex> guard(m_inboundMutex);
                        if (m_inbound.size() < MAX_INBOUND)
                        {
                            m_inbound.push_back(InboundMessage{state->getId(), message->str});
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
                                           "hub: cannot serve on tcp/%u: %s\n",
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

    void WsBridge::broadcastBinary(const std::string &bytes)
    {
        if (!m_server)
        {
            return;
        }
        for (const std::shared_ptr<ix::WebSocket> &client : m_server->getClients())
        {
            static_cast<void>(client->sendBinary(bytes));
        }
    }

    std::vector<InboundMessage> WsBridge::drainInbound()
    {
        std::vector<InboundMessage> taken;
        const std::lock_guard<std::mutex> guard(m_inboundMutex);
        taken.swap(m_inbound);
        return taken;
    }
} // namespace mark4
