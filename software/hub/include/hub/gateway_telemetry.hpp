#pragma once

/// @file
/// @brief The telemetry concept on the websocket side: the measure tables,
///        the configuration as each node applied it, and the sample stream
///        the clients that asked for it receive.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_publisher.hpp"
#include "telemetry/consumer.hpp"

namespace mark4
{
    /// Publishes what the telemetry consumer holds and drives it from the
    /// clients' commands. There is one configuration per node, so the last
    /// client to write it wins; the sample stream, on the other hand, is
    /// held while at least one client asked for it and goes to those clients
    /// alone.
    class TelemetryGateway final : public AbsTelemetryConsumerListener
    {
      public:
        /// @param publisher where the messages go
        /// @param consumer consumer this gateway listens to and drives
        TelemetryGateway(AbsGatewayPublisher &publisher, TelemetryConsumerBase &consumer);

        /// @brief One node's measure table changed: published whole.
        /// @param nodeId the node
        /// @param descriptors its table, as the consumer holds it
        void onTable(std::uint32_t nodeId,
                     std::span<const mark4_TelemetryDescriptor> descriptors) override;

        /// @brief The configuration one node applied, and whether the
        ///        gateway holds its stream.
        /// @param nodeId the node
        /// @param config the configuration as applied
        /// @param subscribed true while the gateway holds the stream
        void onConfig(std::uint32_t nodeId,
                      const mark4_TelemetryConfig &config,
                      bool subscribed) override;

        /// @brief One sampling instant, to the clients marked on that node.
        /// @param nodeId the node
        /// @param data the samples
        void onSamples(std::uint32_t nodeId, const mark4_TelemetryData &data) override;

        /// @brief A node went down: its marks go, and its table is published
        ///        empty because the ids of the next boot are its own.
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override;

        /// @brief A client connected: every complete table and every
        ///        configuration known, to that client alone.
        /// @param clientId the client
        void onClientConnected(const std::string &clientId);

        /// @brief A client went away: its marks go with it, and the stream
        ///        of a node nobody watches any more is given back.
        /// @param clientId the client
        void onClientClosed(const std::string &clientId);

        /// @brief Carries out one telemetry command: a configuration for the
        ///        node, or this client asking for its samples.
        /// @param command what the client asked
        /// @param clientId connection it came from
        /// @param[out] errorOut receives the refusal reason
        /// @return true when it was carried out
        bool apply(const mark4_TelemetryCommand &command,
                   const std::string &clientId,
                   std::string &errorOut);

      private:
        /// @brief Publishes one node's measure table, as the consumer holds
        ///        it.
        /// @param nodeId the node
        /// @param descriptors its table
        void publishTable(std::uint32_t nodeId,
                          std::span<const mark4_TelemetryDescriptor> descriptors);

        /// @brief Publishes one node's configuration as applied.
        /// @param nodeId the node
        /// @param config the configuration
        /// @param subscribed true while the gateway holds the stream
        /// @param clientId client to send it to, empty to broadcast
        void publishConfig(std::uint32_t nodeId,
                           const mark4_TelemetryConfig &config,
                           bool subscribed,
                           const std::string &clientId);

        /// @brief Marks or unmarks one client on one node and follows the
        ///        subscription: held while at least one client is marked,
        ///        given back when the last one clears.
        /// @param nodeId the node
        /// @param clientId the client
        /// @param wanted true to mark it
        void mark(std::uint32_t nodeId, const std::string &clientId, bool wanted);

        AbsGatewayPublisher &m_publisher;  ///< where the messages go
        TelemetryConsumerBase &m_consumer; ///< what this gateway publishes and drives
        /// Clients that asked for the samples of a node, by node id: the
        /// stream is held while a set is not empty.
        std::map<std::uint32_t, std::set<std::string>> m_marks;
    };
} // namespace mark4
