#pragma once

/// @file
/// @brief Per-link health the hub publishes: the frame counters the
///        transport keeps for every node it hears, named after the node
///        kind discovery gave them.

#include <cstdint>
#include <string>

namespace mark4
{
    /// Counters of one transport node.
    struct LinkHealth
    {
        std::uint8_t sourceId = 0U;      ///< NodeKind of the sender
        std::string sourceName;          ///< name discovery gives that kind
        std::uint64_t received = 0U;     ///< frames seen, duplicates included
        std::uint64_t lost = 0U;         ///< frames the numbering says never arrived
        std::uint64_t duplicates = 0U;   ///< frames carrying the previous number
        std::uint16_t lastSequence = 0U; ///< number of the last frame seen
    };

    /// @brief Share of the frames a link should have carried that never
    ///        arrived.
    /// @param link link to score
    /// @return the ratio in [0, 1], 0 when nothing was expected yet
    double linkLossRate(const LinkHealth &link);
} // namespace mark4
