#pragma once

/// @file
/// @brief Default UDP ports of every process boundary, in one place. A
///        deployment may override them (batch campaigns stride their own
///        ranges), but every default and every port relationship is
///        defined here and nowhere else.

#include <cstdint>

namespace mark4
{
    /// UDP port the flight process listens on for sensor packets. Actuator
    /// packets are sent back to the address and port the last sensor packet
    /// came from, so the simulator needs no listening port of its own.
    inline constexpr std::uint16_t SIM_LINK_PORT = 47800U;

    /// UDP port telemetry is broadcast to; any number of consumers may listen,
    /// provided their socket sets SO_REUSEADDR so the port can be shared.
    inline constexpr std::uint16_t TELEMETRY_PORT = 47801U;

    /// UDP port the simulator broadcasts its raw state to. Distinct from the
    /// telemetry port so consumers can compare the estimated state (telemetry)
    /// with the exact one (this stream) sample by sample.
    inline constexpr std::uint16_t SIM_RAW_PORT = 47802U;

    // 47803 is unassigned: it used to mirror the telemetry broadcast for the
    // one consumer whose socket stack could not share a bound port. Every
    // consumer left sets SO_REUSEADDR and reads udp/47801 directly.

    // 47804 is unassigned: the simulator used to bind it for scenario
    // commands, and binds no listening port at all any more. Scenarios reach
    // it inside the lockstep reply.

    /// UDP port a flight process binds its command receiver to. It carries
    /// the pilot RC stream (RcCommandPacket) and the scenario commands
    /// (SimScenarioPacket) the flight process forwards to its plant, plus
    /// the commands that follow them. Ownership rule: the receiving flight
    /// process binds, senders just send. drone_sim binds it by default; the
    /// hub binds it when it is the one fronting the real board.
    inline constexpr std::uint16_t RC_COMMAND_PORT = 47805U;

    /// UDP port every flight process broadcasts its AnnouncePacket to. One
    /// shared port for every instance and every kind: the packet itself
    /// carries the ports that matter, so batch campaigns never stride this
    /// one. Consumers bind with SO_REUSEADDR so several watchers coexist.
    inline constexpr std::uint16_t ANNOUNCE_PORT = 47806U;
} // namespace mark4
