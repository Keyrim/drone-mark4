#pragma once

/// @file
/// @brief What one simulated run amounted to: the hash of the trajectory it
///        produced and the health of the link that carried it. Broadcast on
///        the telemetry port next to the telemetry stream, so a campaign
///        reads its verdict from the same socket it judges from.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
    /// The link lost a tick during the run: the trajectory is no longer the
    /// one the scenario asked for, whatever the hash says.
    inline constexpr std::uint8_t SIM_RUN_FLAG_LOCKSTEP_DEGRADED = 0x01U;

    /// The hash window elapsed: runHash is final and may be compared.
    inline constexpr std::uint8_t SIM_RUN_FLAG_HASH_SEALED = 0x02U;

#pragma pack(push, 1)
    /// One run, boiled down to numbers a campaign can compare. Streamed like
    /// the telemetry (source id then sequence), republished on every change
    /// and periodically otherwise, so a consumer that joined late still
    /// learns where the run stands.
    struct SimRunStatsPacket
    {
        std::uint8_t version;           ///< = PROTOCOL_VERSION
        std::uint8_t type;              ///< = PacketType::SIM_RUN_STATS
        std::uint8_t sourceId;          ///< StreamSource of the sender
        std::uint16_t sequence;         ///< increments per packet sent, wraps
        std::uint8_t runId;             ///< reset counter of the measured run
        std::uint8_t flags;             ///< SIM_RUN_FLAG_* bits
        std::uint64_t runStartUs;       ///< simulated time the run started at [us]
        std::uint64_t runHash;          ///< hash of the trajectory so far, final
                                        ///< once SIM_RUN_FLAG_HASH_SEALED is set
        std::uint32_t duplicateFrames;  ///< sensor resends answered again rather
                                        ///< than stepped, cumulative
        std::uint32_t lockstepTimeouts; ///< lockstep timeouts reported by the
                                        ///< plant, cumulative
    };
#pragma pack(pop)

    /// version (1) + type (1) + source (1) + sequence (2) + run id (1)
    /// + flags (1) + run start (8) + hash (8) + duplicates (4) + timeouts (4).
    inline constexpr std::size_t SIM_RUN_STATS_PACKET_SIZE = 31U;

    static_assert(sizeof(SimRunStatsPacket) == SIM_RUN_STATS_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimRunStatsPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(SimRunStatsPacket, version) == 0U);
    static_assert(offsetof(SimRunStatsPacket, type) == 1U);
    static_assert(offsetof(SimRunStatsPacket, sourceId) == 2U);
    static_assert(offsetof(SimRunStatsPacket, sequence) == 3U);
    static_assert(offsetof(SimRunStatsPacket, runId) == 5U);
    static_assert(offsetof(SimRunStatsPacket, flags) == 6U);
    static_assert(offsetof(SimRunStatsPacket, runStartUs) == 7U);
    static_assert(offsetof(SimRunStatsPacket, runHash) == 15U);
    static_assert(offsetof(SimRunStatsPacket, duplicateFrames) == 23U);
    static_assert(offsetof(SimRunStatsPacket, lockstepTimeouts) == 27U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
