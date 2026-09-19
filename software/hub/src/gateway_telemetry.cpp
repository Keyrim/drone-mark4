/// @file
/// @brief Telemetry gateway implementation.

#include "hub/gateway_telemetry.hpp"

#include <algorithm>
#include <cstddef>

#include "hub/gateway_codec.hpp"

namespace mark4
{
    TelemetryGateway::TelemetryGateway(AbsGatewayPublisher &publisher,
                                       TelemetryConsumerBase &consumer)
        : AbsTelemetryConsumerListener(consumer),
          m_publisher(publisher),
          m_consumer(consumer)
    {
    }

    void TelemetryGateway::publishTable(std::uint32_t nodeId,
                                        std::span<const mark4_TelemetryDescriptor> descriptors)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_telemetry_tag;
        fillNodeTelemetry(nodeId, descriptors, message.body.node_telemetry);
        m_publisher.broadcast(message);
    }

    void TelemetryGateway::publishConfig(std::uint32_t nodeId,
                                         const mark4_TelemetryConfig &config,
                                         bool subscribed,
                                         const std::string &clientId)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_telemetry_config_tag;
        mark4_NodeTelemetryConfig &published = message.body.node_telemetry_config;
        published.node = nodeId;
        const std::size_t count =
            std::min(static_cast<std::size_t>(config.ids_count), std::size(published.ids));
        std::copy_n(std::begin(config.ids), count, std::begin(published.ids));
        published.ids_count = static_cast<pb_size_t>(count);
        published.period_ms = config.period_ms;
        published.subscribed = subscribed;
        if (clientId.empty())
        {
            m_publisher.broadcast(message);
            return;
        }
        m_publisher.sendTo(clientId, message);
    }

    void TelemetryGateway::onTable(std::uint32_t nodeId,
                                   std::span<const mark4_TelemetryDescriptor> descriptors)
    {
        publishTable(nodeId, descriptors);
    }

    void TelemetryGateway::onConfig(std::uint32_t nodeId,
                                    const mark4_TelemetryConfig &config,
                                    bool subscribed)
    {
        publishConfig(nodeId, config, subscribed, "");
    }

    void TelemetryGateway::onSamples(std::uint32_t nodeId, const mark4_TelemetryData &data)
    {
        const auto marked = m_marks.find(nodeId);
        if (marked == m_marks.end())
        {
            return;
        }
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_telemetry_samples_tag;
        message.body.telemetry_samples.node = nodeId;
        message.body.telemetry_samples.has_data = true;
        message.body.telemetry_samples.data = data;
        for (const std::string &clientId : marked->second)
        {
            m_publisher.sendTo(clientId, message);
        }
    }

    void TelemetryGateway::onForgotten(std::uint32_t nodeId)
    {
        // The ids of a telemetry table are only stable while the node runs,
        // so the clients are told at once, with an empty table, rather than
        // keeping curves bound to ids the next boot hands to other measures.
        m_marks.erase(nodeId);
        publishTable(nodeId, {});
    }

    void TelemetryGateway::onClientConnected(const std::string &clientId)
    {
        for (std::size_t index = 0U; index < m_consumer.size(); ++index)
        {
            const TelemetryConsumerBase::Entry &entry = m_consumer.entry(index);
            if (entry.table.complete())
            {
                mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
                message.which_body = mark4_GatewayMessage_node_telemetry_tag;
                fillNodeTelemetry(entry.id, entry.table.items(), message.body.node_telemetry);
                m_publisher.sendTo(clientId, message);
            }
            if (entry.hasConfig)
            {
                publishConfig(entry.id, entry.config, entry.subscribed, clientId);
            }
        }
    }

    void TelemetryGateway::onClientClosed(const std::string &clientId)
    {
        // A tab that went away must not leave a node streaming to nobody.
        for (auto &[nodeId, clients] : m_marks)
        {
            if (clients.erase(clientId) != 0U && clients.empty())
            {
                static_cast<void>(m_consumer.subscribe(nodeId, false));
            }
        }
    }

    void TelemetryGateway::mark(std::uint32_t nodeId, const std::string &clientId, bool wanted)
    {
        std::set<std::string> &clients = m_marks[nodeId];
        const bool held = !clients.empty();
        if (wanted)
        {
            clients.insert(clientId);
        }
        else
        {
            static_cast<void>(clients.erase(clientId));
        }
        if (held == !clients.empty())
        {
            return;
        }
        // The stream is held while at least one client wants it, and given
        // back when the last one clears.
        static_cast<void>(m_consumer.subscribe(nodeId, !clients.empty()));
    }

    bool TelemetryGateway::apply(const mark4_TelemetryCommand &command,
                                 const std::string &clientId,
                                 std::string &errorOut)
    {
        switch (command.which_action)
        {
            case mark4_TelemetryCommand_config_tag: {
                const mark4_TelemetryConfigRequest &wanted = command.action.config;
                if (!m_consumer.configure(
                        command.node,
                        std::span<const std::uint32_t>(std::begin(wanted.ids), wanted.ids_count),
                        wanted.period_ms))
                {
                    errorOut = "node " + hexNodeId(command.node) + " took no configuration";
                    return false;
                }
                return true;
            }
            case mark4_TelemetryCommand_subscribe_tag:
                if (m_consumer.find(command.node) == nullptr)
                {
                    errorOut = "node " + hexNodeId(command.node) + " has no telemetry here";
                    return false;
                }
                mark(command.node, clientId, command.action.subscribe);
                return true;
            default:
                errorOut = "empty telemetry command";
                return false;
        }
    }
} // namespace mark4
