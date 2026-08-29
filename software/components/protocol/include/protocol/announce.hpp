#pragma once

/// @file
/// @brief Process announce packet, the basis of ground-side discovery.
///        Every flight process broadcasts it periodically and the hub
///        discovers whoever is alive instead of relying on hand-wired
///        ports.

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
#pragma pack(push, 1)
    /// One process saying "here I am": its kind, the ports it serves and
    /// (via the version byte) the protocol it speaks.
    struct AnnouncePacket
    {
        std::uint8_t version;        ///< = PROTOCOL_VERSION
        std::uint8_t type;           ///< = PacketType::ANNOUNCE
        std::uint8_t kind;           ///< StreamSource of the announcing process
        std::uint32_t sessionId;     ///< identity of this process start, drawn
                                     ///< once at startup, 0 when the process
                                     ///< assigns none; lets a consumer detect
                                     ///< a restart behind an unchanged kind
                                     ///< and port pair
        std::uint16_t telemetryPort; ///< port the process broadcasts telemetry
                                     ///< to, 0 when it has none
        std::uint16_t commandPort;   ///< port the process listens on for
                                     ///< commands, 0 when it has none
    };
#pragma pack(pop)

    /// version (1) + type (1) + kind (1) + session (4) + telemetry port (2)
    /// + command port (2).
    inline constexpr std::size_t ANNOUNCE_PACKET_SIZE = 11U;

    static_assert(sizeof(AnnouncePacket) == ANNOUNCE_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<AnnouncePacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(AnnouncePacket, kind) == 2U);
    static_assert(offsetof(AnnouncePacket, sessionId) == 3U);
    static_assert(offsetof(AnnouncePacket, telemetryPort) == 7U);
    static_assert(offsetof(AnnouncePacket, commandPort) == 9U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
