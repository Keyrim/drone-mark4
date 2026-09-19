#pragma once

/// @file
/// @brief What a gateway needs of the endpoint: one way to reach every
///        client and one way to reach a single one. The gateways know
///        nothing of the websocket, of the codec or of the composition
///        around them.

#include <string>

#include "gateway.pb.h"

namespace mark4
{
    /// Encodes one GatewayMessage and sends it out. Implemented by the
    /// composition (which owns the endpoint); every gateway holds one by
    /// reference. Inline bodies like every other abstract class of the
    /// project.
    class AbsGatewayPublisher
    {
      public:
        AbsGatewayPublisher() = default;
        virtual ~AbsGatewayPublisher() = default;

        AbsGatewayPublisher(const AbsGatewayPublisher &) = delete;
        AbsGatewayPublisher &operator=(const AbsGatewayPublisher &) = delete;
        AbsGatewayPublisher(AbsGatewayPublisher &&) = delete;
        AbsGatewayPublisher &operator=(AbsGatewayPublisher &&) = delete;

        /// @brief Sends one message to every connected client.
        /// @param message message to publish
        virtual void broadcast(const mark4_GatewayMessage &message) = 0;

        /// @brief Sends one message to one client: what it alone asked for,
        ///        and the snapshot it gets when it connects.
        /// @param clientId connection to reach
        /// @param message message to publish
        virtual void sendTo(const std::string &clientId, const mark4_GatewayMessage &message) = 0;
    };
} // namespace mark4
