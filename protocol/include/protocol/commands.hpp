#pragma once

/// @file
/// @brief Scenario commands sent to the simulator, for scripted campaigns.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mark4
{
    /// UDP port the simulator listens on for scenario commands.
    inline constexpr std::uint16_t SIM_COMMAND_PORT = 47804U;

    /// UDP port the serial bridge (PC script today, ESP32 later) listens
    /// on for the pilot RC commands to relay to the real board. Distinct
    /// from SIM_COMMAND_PORT: the simulator binds its port exclusively,
    /// and the ghost view use case runs both at once.
    inline constexpr std::uint16_t RC_COMMAND_PORT = 47805U;

    /// Teleports the drone back to its start pose, velocities zeroed. The
    /// flight process restarts through the reset counter of the sim link.
    inline constexpr std::uint8_t SIM_COMMAND_RESET = 1U;

    /// Applies the RC fields of the packet as the pilot state.
    inline constexpr std::uint8_t SIM_COMMAND_RC = 2U;

    /// Plays an instant throw with the velocity and rotation of the packet.
    inline constexpr std::uint8_t SIM_COMMAND_THROW = 3U;

    /// Plays a full hand sequence: pick the drone up into the given held
    /// attitude, hold it (with the sway of a real arm), then swing and
    /// release at the packet velocity. A zero swing duration holds forever:
    /// the scenario measuring false spin-ups in a shaken hand.
    inline constexpr std::uint8_t SIM_COMMAND_HAND_THROW = 4U;

#pragma pack(push, 1)
    /// One scenario command. A single layout for every command keeps the
    /// framing trivial; fields unused by the command are ignored. Vectors use
    /// the drone frame convention of the protocol (body x forward, y left,
    /// z up; world z up).
    struct SimCommandPacket
    {
        std::uint8_t version;                     ///< = PROTOCOL_VERSION
        std::uint8_t command;                     ///< one of the SIM_COMMAND_* values
        std::uint8_t killSwitch;                  ///< RC: 1 = engaged (motors cut)
        std::uint8_t armSwitch;                   ///< RC: 1 = armed for a throw flight
        float throttle;                           ///< RC: normalized [0, 1]
        std::array<float, 3> velocityMps;         ///< (HAND_)THROW: release velocity, world [m/s]
        std::array<float, 3> angularVelocityRadS; ///< (HAND_)THROW: release spin, body [rad/s]
        float heldSeconds;                        ///< HAND_THROW: hold before the swing [s]
        float heldTiltRad;                        ///< HAND_THROW: held tilt from level [rad]
        float heldAzimuthRad;                     ///< HAND_THROW: world azimuth of the tilt [rad]
        float swingSeconds;                       ///< HAND_THROW: swing duration, 0 = hold only [s]
    };
#pragma pack(pop)

    /// version (1) + command (1) + kill switch (1) + arm switch (1)
    /// + throttle (4) + velocity (12) + angular velocity (12) + held (4)
    /// + held tilt (4) + held azimuth (4) + swing (4).
    inline constexpr std::size_t SIM_COMMAND_PACKET_SIZE = 48U;

    static_assert(sizeof(SimCommandPacket) == SIM_COMMAND_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimCommandPacket>);

#pragma pack(push, 1)
    /// Pilot state for the firmware uplink. Streamed periodically (10 Hz),
    /// never fired once: the firmware treats a silent link as a kill
    /// (fail-safe), so holding a state means repeating it.
    struct RcCommandPacket
    {
        std::uint8_t version;    ///< = PROTOCOL_VERSION
        std::uint8_t killSwitch; ///< 1 = engaged (motors cut)
        std::uint8_t armSwitch;  ///< 1 = armed for an autonomous throw flight
        float throttle;          ///< normalized [0, 1]
    };
#pragma pack(pop)

    /// version (1) + kill switch (1) + arm switch (1) + throttle (4).
    inline constexpr std::size_t RC_COMMAND_PACKET_SIZE = 7U;

    static_assert(sizeof(RcCommandPacket) == RC_COMMAND_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<RcCommandPacket>);
} // namespace mark4
