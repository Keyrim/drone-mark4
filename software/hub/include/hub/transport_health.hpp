#pragma once

/// @file
/// @brief What the gateway makes of the transport reports it holds: the
///        rates of one node over a sliding window, and the verdict on the
///        whole wire read from every view at once. Pure arithmetic, samples
///        in, messages out: no publisher, no consumer, no state.

#include <cstdint>
#include <span>
#include <vector>

#include "gateway.pb.h"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// One complete report of one node, as it is kept in the window the
    /// rates are derived over.
    struct TransportSample
    {
        std::uint64_t instantUs = 0U; ///< instant the report was complete [us]
        /// the counters and the links as the node reported them; its peers
        /// field is unused, the table below holds them
        mark4_TransportReport report = mark4_TransportReport_init_zero;
        std::vector<mark4_TransportPeer> peers; ///< the peer table of that report, pages merged
    };

    /// Loss on one edge above this is DEGRADED. A pilot link that drops one
    /// frame in a hundred is already worth a colour.
    inline constexpr float LOSS_DEGRADED = 0.01f;

    /// Loss on one edge above this is BAD: a tenth of what is sent is gone.
    inline constexpr float LOSS_BAD = 0.10f;

    /// A peer nobody heard from for this long is fading [ms]: the keepalive
    /// is once a second, so a silence longer than that is already one
    /// missed.
    inline constexpr std::uint32_t FADING_MS = 1500U;

    /// @brief Derives one node's view: its last report, and what its
    ///        counters did between the two samples given.
    /// @param nodeId node the samples came from
    /// @param newest its last report
    /// @param oldest the oldest report of the window; the same sample as
    ///        newest when only one is held, which leaves every rate and
    ///        every window counter at zero
    /// @param[out] out receives the message
    void fillNodeTransport(std::uint32_t nodeId,
                           const TransportSample &newest,
                           const TransportSample &oldest,
                           mark4_NodeTransport &out);

    /// @brief Judges the whole transport from every view held: one verdict
    ///        per node of the table, and the worst of them for the system.
    /// @param nodeIds the node table, the gateway first
    /// @param views the view of every node a complete report is held of
    /// @param[out] out receives the verdict
    void fillTransportHealth(std::span<const std::uint32_t> nodeIds,
                             std::span<const mark4_NodeTransport> views,
                             mark4_TransportHealth &out);
} // namespace mark4
