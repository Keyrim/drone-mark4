#pragma once

/// @file
/// @brief Default UDP port of the one boundary that carries a bare
///        Envelope outside the transport: the lockstep link between a
///        flight process and its plant. Everything between the flight
///        processes and the hub travels through the transport, whose one
///        shared port is transport/udp_link.hpp (DISCOVERY_PORT). A
///        deployment may override this (batch campaigns stride their own
///        ranges), but the default is defined here and nowhere else.

#include <cstdint>

namespace mark4
{
    /// UDP port the flight process listens on for SimSensor envelopes.
    /// Its SimActuator and SimScenario envelopes go back to the address and
    /// port the last sensor came from, so the plant needs no listening port
    /// of its own.
    inline constexpr std::uint16_t SIM_LINK_PORT = 47800U;
} // namespace mark4
