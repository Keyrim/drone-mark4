/// @file
/// @brief Status gateway implementation.

#include "hub/gateway_status.hpp"

#include <cstddef>

namespace mark4
{
    StatusGateway::StatusGateway(AbsGatewayPublisher &publisher, StatusConsumerBase &consumer)
        : AbsStatusConsumerListener(consumer),
          m_publisher(publisher),
          m_consumer(consumer)
    {
    }

    mark4_GatewayMessage StatusGateway::Message(std::uint32_t nodeId, const mark4_Status &status)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_status_tag;
        message.body.node_status.node = nodeId;
        message.body.node_status.has_status = true;
        message.body.node_status.status = status;
        return message;
    }

    void StatusGateway::onStatus(std::uint32_t nodeId,
                                 const mark4_Status &status,
                                 std::uint64_t nowUs)
    {
        static_cast<void>(nowUs);
        m_publisher.broadcast(Message(nodeId, status));
    }

    void StatusGateway::onForgotten(std::uint32_t nodeId)
    {
        static_cast<void>(nodeId);
    }

    void StatusGateway::onClientConnected(const std::string &clientId)
    {
        for (std::size_t index = 0U; index < m_consumer.size(); ++index)
        {
            const StatusConsumerBase::Entry &entry = m_consumer.entry(index);
            if (entry.hasStatus)
            {
                m_publisher.sendTo(clientId, Message(entry.id, entry.last));
            }
        }
    }
} // namespace mark4
