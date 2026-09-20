/// @file
/// @brief Transport gateway implementation.

#include "hub/gateway_transport.hpp"

#include <iterator>
#include <utility>
#include <vector>

#include "log/module.hpp"
#include "log_modules.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_GATEWAY_TRANSPORT, "gateway/transport"};
    } // namespace

    TransportGateway::TransportGateway(AbsGatewayPublisher &publisher,
                                       TransportConsumerBase &consumer,
                                       const TransportProvider &provider,
                                       std::uint32_t selfId)
        : AbsTransportConsumerListener(consumer),
          m_publisher(publisher),
          m_consumer(consumer),
          m_provider(provider),
          m_selfId(selfId)
    {
        if (!m_consumer.openLocal(selfId))
        {
            MODULE.warn("no room for this node's own transport view");
        }
    }

    void TransportGateway::onReport(std::uint32_t nodeId,
                                    const TransportConsumerBase::Entry &entry,
                                    std::uint64_t nowUs)
    {
        TransportSample sample;
        sample.instantUs = nowUs;
        sample.report = entry.last;
        sample.peers.assign(entry.peers.data(), entry.peers.data() + entry.peerCount);
        std::deque<TransportSample> &samples = m_samples[nodeId];
        samples.push_back(std::move(sample));
        while (samples.size() > WINDOW)
        {
            samples.pop_front();
        }
        publishView(nodeId, samples, "");
    }

    void TransportGateway::onForgotten(std::uint32_t nodeId)
    {
        m_samples.erase(nodeId);
    }

    void TransportGateway::onClientConnected(const std::string &clientId)
    {
        for (const auto &[nodeId, samples] : m_samples)
        {
            if (!samples.empty())
            {
                publishView(nodeId, samples, clientId);
            }
        }
        if (m_judged)
        {
            mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
            message.which_body = mark4_GatewayMessage_transport_health_tag;
            message.body.transport_health = m_health;
            m_publisher.sendTo(clientId, message);
        }
    }

    void TransportGateway::onClientClosed(const std::string &clientId)
    {
        // A tab that went away must not leave every node reporting to
        // nobody.
        mark(clientId, false);
    }

    bool TransportGateway::apply(const mark4_TransportCommand &command,
                                 const std::string &clientId,
                                 std::string &errorOut)
    {
        static_cast<void>(errorOut);
        mark(clientId, command.subscribe);
        return true;
    }

    void TransportGateway::tick(std::uint64_t nowUs)
    {
        // A messenger refuses this node as a destination, so its own report
        // does not travel: the pages go straight from its provider into its
        // consumer, exactly as the wire would deliver them.
        mark4_TransportReport page = mark4_TransportReport_init_zero;
        std::uint32_t cursor = 0U;
        do
        {
            const std::size_t written = m_provider.fillPage(cursor, nowUs, page);
            m_consumer.accept(m_selfId, page, nowUs);
            cursor += static_cast<std::uint32_t>(written);
        } while (cursor < page.peer_total);
        publishHealth();
    }

    void TransportGateway::mark(const std::string &clientId, bool wanted)
    {
        const bool held = !m_clients.empty();
        if (wanted)
        {
            m_clients.insert(clientId);
        }
        else
        {
            static_cast<void>(m_clients.erase(clientId));
        }
        if (held == !m_clients.empty())
        {
            return;
        }
        m_consumer.setWanted(!m_clients.empty());
        if (m_clients.empty())
        {
            MODULE.info("no client watches the wire any more: the reports are given back");
            forgetRemote();
            return;
        }
        MODULE.info("a client watches the wire: every node is asked for its reports");
    }

    void TransportGateway::forgetRemote()
    {
        for (auto sample = m_samples.begin(); sample != m_samples.end();)
        {
            sample = sample->first == m_selfId ? std::next(sample) : m_samples.erase(sample);
        }
    }

    void TransportGateway::publishView(std::uint32_t nodeId,
                                       const std::deque<TransportSample> &samples,
                                       const std::string &clientId)
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_node_transport_tag;
        fillNodeTransport(nodeId, samples.back(), samples.front(), message.body.node_transport);
        if (clientId.empty())
        {
            m_publisher.broadcast(message);
            return;
        }
        m_publisher.sendTo(clientId, message);
    }

    void TransportGateway::publishHealth()
    {
        std::vector<mark4_NodeTransport> views;
        views.reserve(m_samples.size());
        for (const auto &[nodeId, samples] : m_samples)
        {
            if (samples.empty())
            {
                continue;
            }
            mark4_NodeTransport &view = views.emplace_back();
            fillNodeTransport(nodeId, samples.back(), samples.front(), view);
        }
        // The node table is this node's own peers, in the order its
        // transport holds them, behind this node itself: the same table the
        // gateway publishes as NodeTable.
        std::vector<std::uint32_t> nodeIds{m_selfId};
        for (const mark4_NodeTransport &view : views)
        {
            if (view.node != m_selfId)
            {
                continue;
            }
            nodeIds.reserve(1U + view.edges_count);
            for (pb_size_t index = 0U; index < view.edges_count; ++index)
            {
                nodeIds.push_back(view.edges[index].peer);
            }
        }
        fillTransportHealth(nodeIds, views, m_health);
        m_judged = true;
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_transport_health_tag;
        message.body.transport_health = m_health;
        m_publisher.broadcast(message);
    }
} // namespace mark4
