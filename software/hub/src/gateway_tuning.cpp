/// @file
/// @brief Tuning gateway implementation.

#include "hub/gateway_tuning.hpp"

#include <algorithm>
#include <cstddef>

#include "hub/gateway_codec.hpp"

namespace mark4
{
    TuningGateway::TuningGateway(AbsGatewayPublisher &publisher, TuningConsumerBase &consumer)
        : AbsTuningConsumerListener(consumer),
          m_publisher(publisher),
          m_consumer(consumer)
    {
    }

    mark4_GatewayMessage TuningGateway::Message(std::uint32_t nodeId,
                                                std::span<const mark4_TuningInfo> infos)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_tuning_tag;
        mark4_NodeTuning &published = message.body.node_tuning;
        published.node = nodeId;
        const std::size_t count = std::min(infos.size(), std::size(published.infos));
        std::copy_n(infos.begin(), count, published.infos);
        published.infos_count = static_cast<pb_size_t>(count);
        return message;
    }

    void TuningGateway::onTable(std::uint32_t nodeId, std::span<const mark4_TuningInfo> infos)
    {
        m_publisher.broadcast(Message(nodeId, infos));
    }

    void TuningGateway::onResult(std::uint32_t nodeId, const mark4_TuningAck &ack)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_tuning_result_tag;
        message.body.tuning_result.node = nodeId;
        message.body.tuning_result.has_ack = true;
        message.body.tuning_result.ack = ack;
        m_publisher.broadcast(message);
    }

    void TuningGateway::onForgotten(std::uint32_t nodeId)
    {
        m_publisher.broadcast(Message(nodeId, {}));
    }

    void TuningGateway::onClientConnected(const std::string &clientId)
    {
        for (std::size_t index = 0U; index < m_consumer.size(); ++index)
        {
            const TuningConsumerBase::Entry &entry = m_consumer.entry(index);
            m_publisher.sendTo(clientId, Message(entry.id, entry.table.items()));
        }
    }

    bool TuningGateway::set(std::uint32_t nodeId, std::uint32_t paramId, float value)
    {
        return m_consumer.set(nodeId, paramId, value);
    }

    bool TuningGateway::apply(const mark4_TuningCommand &command,
                              const std::string &clientId,
                              std::string &errorOut)
    {
        bool taken = false;
        switch (command.which_action)
        {
            case mark4_TuningCommand_refresh_tag:
                taken = m_consumer.refresh(command.node);
                break;
            case mark4_TuningCommand_set_tag:
                taken =
                    m_consumer.set(command.node, command.action.set.id, command.action.set.value);
                break;
            case mark4_TuningCommand_get_tag:
                taken = m_consumer.get(command.node, command.action.get.id);
                break;
            default:
                errorOut = "empty tuning command";
                return false;
        }
        static_cast<void>(clientId);
        if (!taken)
        {
            errorOut = "node " + hexNodeId(command.node) + " took no tuning request";
        }
        return taken;
    }
} // namespace mark4
