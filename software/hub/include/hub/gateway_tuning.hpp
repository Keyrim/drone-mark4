#pragma once

/// @file
/// @brief The tuning concept on the websocket side: the parameter table of
///        each node, and the answer to every write and read.

#include <cstdint>
#include <span>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_publisher.hpp"
#include "tuning/consumer.hpp"

namespace mark4
{
    /// Publishes what the tuning consumer holds and drives it from the
    /// clients' commands. An answer goes to every client: they correlate it
    /// with the id of their own command and with the parameter id.
    class TuningGateway final : public AbsTuningConsumerListener
    {
      public:
        /// @param publisher where the messages go
        /// @param consumer consumer this gateway listens to and drives
        TuningGateway(AbsGatewayPublisher &publisher, TuningConsumerBase &consumer);

        /// @brief One node's parameter table changed: published whole.
        /// @param nodeId the node
        /// @param infos its table, as the consumer holds it
        void onTable(std::uint32_t nodeId, std::span<const mark4_TuningInfo> infos) override;

        /// @brief The answer to a write or a read of one node.
        /// @param nodeId the node
        /// @param ack the answer
        void onResult(std::uint32_t nodeId, const mark4_TuningAck &ack) override;

        /// @brief A node went down: its table is published empty, because
        ///        the parameters of the next boot are its own.
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override;

        /// @brief A client connected: every parameter table the gateway
        ///        holds, to that client alone.
        /// @param clientId the client
        void onClientConnected(const std::string &clientId);

        /// @brief Carries out one tuning command: the table pulled again,
        ///        one parameter written or one read.
        /// @param command what the client asked
        /// @param clientId connection it came from, unused: a parameter
        ///        table is everyone's
        /// @param[out] errorOut receives the refusal reason
        /// @return true when it was carried out
        bool apply(const mark4_TuningCommand &command,
                   const std::string &clientId,
                   std::string &errorOut);

        /// @brief Writes one parameter of one node: what a profile push
        ///        sends through.
        /// @param nodeId node to write to
        /// @param paramId parameter to write
        /// @param value value to write
        /// @return true when the request was taken
        bool set(std::uint32_t nodeId, std::uint32_t paramId, float value);

      private:
        /// @brief Fills the message that publishes one node's table.
        /// @param nodeId node the table belongs to
        /// @param infos the table, truncated to the wire bound
        /// @return the message
        static mark4_GatewayMessage Message(std::uint32_t nodeId,
                                            std::span<const mark4_TuningInfo> infos);

        AbsGatewayPublisher &m_publisher; ///< where the messages go
        TuningConsumerBase &m_consumer;   ///< what this gateway publishes and drives
    };
} // namespace mark4
