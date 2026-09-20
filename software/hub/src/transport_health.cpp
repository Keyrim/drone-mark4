/// @file
/// @brief Transport health arithmetic: one node's rates over its window,
///        then the verdict read from every view at once.

#include "hub/transport_health.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;
        constexpr float US_PER_S = 1000000.0f;

        /// What a link the oldest sample did not declare counted: nothing.
        constexpr mark4_TransportLink NO_LINK = mark4_TransportLink_init_zero;

        /// What a peer the oldest sample did not hold counted: nothing.
        constexpr mark4_TransportPeer NO_PEER = mark4_TransportPeer_init_zero;

        /// @param newer later reading of a cumulative counter
        /// @param older earlier reading of the same counter
        /// @return what it counted between the two readings
        std::uint32_t windowDelta(std::uint32_t newer, std::uint32_t older)
        {
            // Unsigned arithmetic wraps where the counter wrapped, so the
            // difference reads right across the wrap too.
            return newer - older;
        }

        /// @param count what happened over the window
        /// @param spanS span of the window [s]
        /// @return the rate, 0 without a window
        float perSecond(std::uint32_t count, float spanS)
        {
            return spanS > 0.0f ? static_cast<float>(count) / spanS : 0.0f;
        }

        /// @param sample sample to look in
        /// @param id peer to find
        /// @return that peer as the sample held it, zeroed when it held none
        const mark4_TransportPeer &peerOf(const TransportSample &sample, std::uint32_t id)
        {
            for (const mark4_TransportPeer &peer : sample.peers)
            {
                if (peer.id == id)
                {
                    return peer;
                }
            }
            return NO_PEER;
        }

        /// @param edge one directed edge
        /// @return true while its window holds too few frames for its loss
        ///         to mean anything
        bool quiet(const mark4_TransportEdge &edge)
        {
            return edge.window_received + edge.window_lost < LOSS_MIN_FRAMES;
        }

        /// @param views every view held
        /// @param nodeId node to find
        /// @return its view, nullptr when it reports nothing
        const mark4_NodeTransport *viewOf(std::span<const mark4_NodeTransport> views,
                                          std::uint32_t nodeId)
        {
            for (const mark4_NodeTransport &view : views)
            {
                if (view.node == nodeId)
                {
                    return &view;
                }
            }
            return nullptr;
        }

        /// @param view one node's view
        /// @param peer node to look for among its edges
        /// @return true when that node's view lists this peer
        bool listsPeer(const mark4_NodeTransport &view, std::uint32_t peer)
        {
            for (pb_size_t index = 0U; index < view.edges_count; ++index)
            {
                if (view.edges[index].peer == peer)
                {
                    return true;
                }
            }
            return false;
        }

        /// @param health one node, every flag already raised
        /// @param observed true when some reporting node lists it as a peer
        /// @return what the gateway makes of it
        mark4_TransportVerdict verdictOf(const mark4_TransportNodeHealth &health, bool observed)
        {
            if (!health.reporting && !observed)
            {
                // Nothing reports it and nobody hears it: there is nothing
                // to judge, not a healthy node.
                return mark4_TransportVerdict_VERDICT_UNKNOWN;
            }
            const float worstLoss = std::max(health.worst_in_loss, health.worst_out_loss);
            if (worstLoss > LOSS_BAD || health.asymmetric || health.requests_failed)
            {
                return mark4_TransportVerdict_VERDICT_BAD;
            }
            if (worstLoss > LOSS_DEGRADED || health.fading || health.link_refused ||
                health.link_rx_errors || health.churn)
            {
                return mark4_TransportVerdict_VERDICT_DEGRADED;
            }
            return mark4_TransportVerdict_VERDICT_OK;
        }
    } // namespace

    void fillNodeTransport(std::uint32_t nodeId,
                           const TransportSample &newest,
                           const TransportSample &oldest,
                           mark4_NodeTransport &out)
    {
        out = mark4_NodeTransport_init_zero;
        out.node = nodeId;
        out.has_report = true;
        out.report = newest.report;
        // The peers travel as edges, with their rates: the report carries
        // the counters and the links alone.
        out.report.peers_count = 0U;
        const std::uint64_t spanUs =
            newest.instantUs - std::min(newest.instantUs, oldest.instantUs);
        out.window_ms = static_cast<std::uint32_t>(spanUs / US_PER_MS);
        const float spanS = static_cast<float>(spanUs) / US_PER_S;

        out.links_count = newest.report.links_count;
        for (pb_size_t index = 0U; index < newest.report.links_count; ++index)
        {
            const mark4_TransportLink &now = newest.report.links[index];
            const mark4_TransportLink &was =
                index < oldest.report.links_count ? oldest.report.links[index] : NO_LINK;
            mark4_TransportLinkRate &rate = out.links[index];
            rate.frames_in_per_s = perSecond(windowDelta(now.frames_in, was.frames_in), spanS);
            rate.bytes_in_per_s = perSecond(windowDelta(now.bytes_in, was.bytes_in), spanS);
            rate.frames_out_per_s = perSecond(windowDelta(now.frames_out, was.frames_out), spanS);
            rate.bytes_out_per_s = perSecond(windowDelta(now.bytes_out, was.bytes_out), spanS);
            rate.refused = windowDelta(now.refused, was.refused);
            rate.rx_errors = windowDelta(now.rx_errors, was.rx_errors);
        }

        for (const mark4_TransportPeer &peer : newest.peers)
        {
            if (out.edges_count >= std::size(out.edges))
            {
                break;
            }
            const mark4_TransportPeer &was = peerOf(oldest, peer.id);
            mark4_TransportEdge &edge = out.edges[out.edges_count];
            ++out.edges_count;
            edge.peer = peer.id;
            edge.link = peer.link;
            edge.hops = peer.hops;
            edge.age_ms = peer.age_ms;
            edge.received = peer.received;
            edge.lost = peer.lost;
            edge.duplicates = peer.duplicates;
            const std::uint32_t received = windowDelta(peer.received, was.received);
            const std::uint32_t lost = windowDelta(peer.lost, was.lost);
            edge.unicast_heard = peer.unicast_heard;
            edge.window_received = received;
            edge.window_lost = lost;
            edge.rx_per_s = perSecond(received, spanS);
            // Loss is what the window missed of what it should have had;
            // with nothing at all in the window it is not a loss.
            edge.loss = received + lost > 0U
                            ? static_cast<float>(lost) / static_cast<float>(received + lost)
                            : 0.0f;
            edge.duplicates_per_s = perSecond(windowDelta(peer.duplicates, was.duplicates), spanS);
        }

        out.has_window = true;
        mark4_TransportWindow &window = out.window;
        window.sent = windowDelta(newest.report.sent, oldest.report.sent);
        window.refused = windowDelta(newest.report.refused, oldest.report.refused);
        window.dropped = windowDelta(newest.report.dropped, oldest.report.dropped);
        window.relayed = windowDelta(newest.report.relayed, oldest.report.relayed);
        window.expired = windowDelta(newest.report.expired, oldest.report.expired);
        window.restarted = windowDelta(newest.report.restarted, oldest.report.restarted);
        window.requests = windowDelta(newest.report.requests, oldest.report.requests);
        window.resent = windowDelta(newest.report.resent, oldest.report.resent);
        window.failed = windowDelta(newest.report.failed, oldest.report.failed);
        window.undecodable = windowDelta(newest.report.undecodable, oldest.report.undecodable);
        window.unhandled = windowDelta(newest.report.unhandled, oldest.report.unhandled);
    }

    void fillTransportHealth(std::span<const std::uint32_t> nodeIds,
                             std::span<const mark4_NodeTransport> views,
                             mark4_TransportHealth &out)
    {
        out = mark4_TransportHealth_init_zero;
        out.nodes_known = static_cast<std::uint32_t>(nodeIds.size());
        for (const mark4_NodeTransport &view : views)
        {
            for (pb_size_t index = 0U; index < view.edges_count; ++index)
            {
                const mark4_TransportEdge &edge = view.edges[index];
                out.frames_per_s += edge.rx_per_s;
                if (quiet(edge))
                {
                    continue;
                }
                if (edge.loss > out.worst_loss)
                {
                    out.worst_loss = edge.loss;
                    out.worst_observer = view.node;
                    out.worst_peer = edge.peer;
                }
            }
        }

        for (const std::uint32_t nodeId : nodeIds)
        {
            if (out.nodes_count >= std::size(out.nodes))
            {
                break;
            }
            mark4_TransportNodeHealth &health = out.nodes[out.nodes_count];
            ++out.nodes_count;
            health.node = nodeId;
            const mark4_NodeTransport *const own = viewOf(views, nodeId);
            health.reporting = own != nullptr;
            if (own != nullptr)
            {
                ++out.nodes_reporting;
                for (pb_size_t index = 0U; index < own->edges_count; ++index)
                {
                    const mark4_TransportEdge &edge = own->edges[index];
                    if (!quiet(edge))
                    {
                        health.worst_in_loss = std::max(health.worst_in_loss, edge.loss);
                    }
                }
                for (pb_size_t index = 0U; index < own->links_count; ++index)
                {
                    health.link_refused = health.link_refused || own->links[index].refused > 0U;
                    health.link_rx_errors =
                        health.link_rx_errors || own->links[index].rx_errors > 0U;
                }
                health.requests_failed = own->window.failed > 0U;
                health.churn = own->window.expired + own->window.restarted > 0U;
            }

            bool observed = false;
            for (const mark4_NodeTransport &view : views)
            {
                if (view.node == nodeId)
                {
                    continue;
                }
                for (pb_size_t index = 0U; index < view.edges_count; ++index)
                {
                    const mark4_TransportEdge &edge = view.edges[index];
                    if (edge.peer != nodeId)
                    {
                        continue;
                    }
                    observed = true;
                    if (!quiet(edge))
                    {
                        health.worst_out_loss = std::max(health.worst_out_loss, edge.loss);
                    }
                    // A quiet edge is still an edge: its age says whether the
                    // node is there, which is what a keepalive is for.
                    health.fading = health.fading || edge.age_ms > FADING_MS;
                    // One side of a pair hearing the other and not the way
                    // back is the shape of a half-open link.
                    health.asymmetric =
                        health.asymmetric || (own != nullptr && !listsPeer(*own, view.node));
                }
            }
            health.verdict = verdictOf(health, observed);
            out.verdict = std::max(out.verdict, health.verdict);
        }
        if (nodeIds.size() <= 1U)
        {
            // The gateway alone on the wire judges nothing: it is the only
            // view there is, and it has no peer to compare with.
            out.verdict = mark4_TransportVerdict_VERDICT_UNKNOWN;
        }
    }
} // namespace mark4
