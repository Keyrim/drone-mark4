#pragma once

/// @file
/// @brief The Status concept on the websocket side: what the Status
///        consumer holds, published as NodeStatus.

#include <cstdint>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_publisher.hpp"
#include "status/consumer.hpp"

namespace mark4
{
    /// Publishes the report of every node the Status consumer follows: one
    /// NodeStatus per report at the node's own cadence, and the last one of
    /// every node to a client that connects.
    class StatusGateway final : public AbsStatusConsumerListener
    {
      public:
        /// @param publisher where the messages go
        /// @param consumer consumer this gateway listens to and reads
        StatusGateway(AbsGatewayPublisher &publisher, StatusConsumerBase &consumer);

        /// @brief One report of one node, published as it arrives.
        /// @param nodeId node it came from
        /// @param status the report
        /// @param nowUs instant of the poll that delivered it [us], unused
        void onStatus(std::uint32_t nodeId,
                      const mark4_Status &status,
                      std::uint64_t nowUs) override;

        /// @brief The consumer dropped a node. Nothing to publish: a node
        ///        that goes down leaves the node table, which is what the
        ///        clients key everything on.
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override;

        /// @brief A client connected: the last report of every node that has
        ///        one, to that client alone.
        /// @param clientId the client
        void onClientConnected(const std::string &clientId);

      private:
        /// @brief Fills the message that publishes one node's report.
        /// @param nodeId node the report belongs to
        /// @param status the report
        /// @return the message
        static mark4_GatewayMessage Message(std::uint32_t nodeId, const mark4_Status &status);

        AbsGatewayPublisher &m_publisher; ///< where the messages go
        StatusConsumerBase &m_consumer;   ///< what this gateway publishes
    };
} // namespace mark4
