#pragma once

/// @file
/// @brief Default UDP ports of the boundaries that still carry a bare
///        protocol/ packet: the lockstep link and the raw state stream of
///        the simulator. Everything between the flight processes and the
///        hub travels through the transport, whose one shared port is
///        transport/udp_link.hpp (DISCOVERY_PORT). A deployment may
///        override these (batch campaigns stride their own ranges), but
///        every default is defined here and nowhere else.

#include <cstdint>

namespace mark4
{
    /// UDP port the flight process listens on for sensor packets. Actuator
    /// packets are sent back to the address and port the last sensor packet
    /// came from, so the simulator needs no listening port of its own.
    inline constexpr std::uint16_t SIM_LINK_PORT = 47800U;

    // 47801 is unassigned: telemetry used to be broadcast to it, and now
    // travels as a transport broadcast frame.

    /// UDP port the simulator broadcasts its raw state to, so consumers can
    /// compare the estimated state (telemetry) with the exact one (this
    /// stream) sample by sample. The plant is not a transport node.
    inline constexpr std::uint16_t SIM_RAW_PORT = 47802U;

    // 47803 to 47806 are unassigned: the telemetry mirror, the simulator's
    // scenario port, the command receiver port and the announce port all
    // died with the ports they described. Commands are transport unicasts
    // to the node that beaconed, the announce is that beacon.
} // namespace mark4
