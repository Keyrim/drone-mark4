#pragma once

/// @file
/// @brief The transport concept on the websocket side: every node's view of
///        the wire, the rates the gateway derives from it, and the verdict
///        it draws from every view at once.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_publisher.hpp"
#include "hub/transport_health.hpp"
#include "transport/consumer.hpp"
#include "transport/provider.hpp"

namespace mark4
{
    /// Publishes what the transport consumer holds: one NodeTransport per
    /// complete report and, once a second, the TransportHealth read from
    /// every view at once. A node reports only to whoever subscribed, so
    /// the reports of the other nodes are held while at least one client
    /// asked for them and given back when the last one clears. The
    /// gateway's own view costs the wire nothing: it walks its own
    /// provider's pages into its own consumer, which a messenger cannot do
    /// for it (a node is not a destination of its own).
    class TransportGateway final : public AbsTransportConsumerListener
    {
      public:
        /// Reports one window spans at most: ten seconds of them, which is
        /// long enough for a rate to mean something and short enough to
        /// follow a link going bad.
        static constexpr std::size_t WINDOW = 10U;

        /// @param publisher where the messages go
        /// @param consumer consumer this gateway listens to and drives
        /// @param provider this node's own report, walked page by page
        /// @param selfId this node's id: the entry its own report feeds
        TransportGateway(AbsGatewayPublisher &publisher,
                         TransportConsumerBase &consumer,
                         const TransportProvider &provider,
                         std::uint32_t selfId);

        /// @brief One node's report is complete: kept as a sample of that
        ///        node's window and published with what the window says.
        /// @param nodeId node it came from
        /// @param entry what the consumer holds of that node
        /// @param nowUs instant the last page arrived [us]
        void onReport(std::uint32_t nodeId,
                      const TransportConsumerBase::Entry &entry,
                      std::uint64_t nowUs) override;

        /// @brief A node went down: its window goes with it. Nothing is
        ///        published: a node that leaves leaves the node table, which
        ///        is what the clients key everything on.
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override;

        /// @brief A client connected: every view held and the last verdict,
        ///        to that client alone.
        /// @param clientId the client
        void onClientConnected(const std::string &clientId);

        /// @brief A client went away: its mark goes with it, and the reports
        ///        nobody watches any more are given back.
        /// @param clientId the client
        void onClientClosed(const std::string &clientId);

        /// @brief Carries out one transport command: this client wanting the
        ///        reports of every node, or letting them go.
        /// @param command what the client asked
        /// @param clientId connection it came from
        /// @param[out] errorOut receives the refusal reason; nothing here is
        ///        refused
        /// @return true, always
        bool apply(const mark4_TransportCommand &command,
                   const std::string &clientId,
                   std::string &errorOut);

        /// @brief Reads this node's own report into its own entry, then
        ///        publishes the verdict. Once a second, from the
        ///        composition.
        /// @param nowUs current instant [us]
        void tick(std::uint64_t nowUs);

      private:
        /// @brief Marks or unmarks one client and follows the subscription:
        ///        the reports of every node are held while at least one
        ///        client is marked, and given back when the last one clears.
        /// @param clientId the client
        /// @param wanted true to mark it
        void mark(const std::string &clientId, bool wanted);

        /// @brief Drops the window of every node but this one, so nothing
        ///        stale is published to a client that connects after the
        ///        reports were given back.
        void forgetRemote();

        /// @brief Publishes one node's view, to one client or to every one.
        /// @param nodeId the node
        /// @param samples its window, never empty
        /// @param clientId client to send it to, empty to broadcast
        void publishView(std::uint32_t nodeId,
                         const std::deque<TransportSample> &samples,
                         const std::string &clientId);

        /// @brief Builds the verdict from every view held and broadcasts it.
        void publishHealth();

        AbsGatewayPublisher &m_publisher;    ///< where the messages go
        TransportConsumerBase &m_consumer;   ///< what this gateway publishes and drives
        const TransportProvider &m_provider; ///< this node's own report, not owned
        std::uint32_t m_selfId;              ///< this node's id
        /// Clients that asked for every node's reports: the subscriptions
        /// are held while it is not empty.
        std::set<std::string> m_clients;
        /// The last WINDOW reports of each node, oldest first: the rates are
        /// what the two ends of one window say.
        std::map<std::uint32_t, std::deque<TransportSample>> m_samples;
        mark4_TransportHealth m_health = mark4_TransportHealth_init_zero; ///< the last verdict
        bool m_judged = false; ///< true once a verdict was built
    };
} // namespace mark4
