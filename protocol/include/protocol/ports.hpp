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

    /// @brief Mirror of a telemetry port, for the single consumer whose
    ///        socket stack cannot share a bound port (Godot's UDP sockets
    ///        bind exclusively). Every telemetry packet is also sent there.
    /// @param telemetryPort port the telemetry broadcast goes to
    /// @return port the mirror copy goes to
    constexpr std::uint16_t telemetryMirrorPort(std::uint16_t telemetryPort)
    {
        return static_cast<std::uint16_t>(telemetryPort + 2U);
    }

    /// Mirror of the default telemetry broadcast port.
    inline constexpr std::uint16_t TELEMETRY_MIRROR_PORT = telemetryMirrorPort(TELEMETRY_PORT);

    /// UDP port the simulator listens on for scenario commands.
    inline constexpr std::uint16_t SIM_COMMAND_PORT = 47804U;

    /// UDP port the serial bridge (PC script today, ESP32 later) listens
    /// on for the pilot RC commands to relay to the real board. Distinct
    /// from SIM_COMMAND_PORT: the simulator binds its port exclusively,
    /// and the ghost view use case runs both at once.
    inline constexpr std::uint16_t RC_COMMAND_PORT = 47805U;

    /// UDP port every flight process broadcasts its AnnouncePacket to. One
    /// shared port for every instance and every kind: the packet itself
    /// carries the ports that matter, so batch campaigns never stride this
    /// one. Consumers bind with SO_REUSEADDR so several watchers coexist.
    inline constexpr std::uint16_t ANNOUNCE_PORT = 47806U;
} // namespace mark4
