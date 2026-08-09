#pragma once

/// @file
/// @brief Scenario commands, RC and the board commands. A scenario is what
///        the simulator must play for one run; it reaches the plant inside
///        the lockstep reply (protocol/sim_link.hpp) and reaches a flight
///        process as SimScenarioPacket on its command receiver.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
    /// Resets the world and nothing else: teleport back to the start pose,
    /// velocities zeroed, generators reseeded. The flight process restarts
    /// through the reset counter of the sim link.
    inline constexpr std::uint8_t SIM_SCENARIO_RESET = 1U;

    /// Retired: interactive and scripted RC both travel as RcCommandPacket
    /// to the flight process command receiver since v11; the value stays
    /// reserved so the neighbors keep their numbers.
    inline constexpr std::uint8_t SIM_SCENARIO_RC = 2U;

    /// Resets, then plays an instant throw at the requested velocity and
    /// rotation, throwDelayUs after the reset tick.
    inline constexpr std::uint8_t SIM_SCENARIO_THROW = 3U;

    /// Resets, then plays a full hand sequence throwDelayUs after the reset
    /// tick: pick the drone up into the given held attitude, hold it (with
    /// the sway of a real arm), then swing and release at the requested
    /// velocity. A zero swing duration holds forever: the scenario measuring
    /// false spin-ups in a shaken hand.
    inline constexpr std::uint8_t SIM_SCENARIO_HAND_THROW = 4U;

    /// Piloting mode carried next to the RC state. Reserved: modes are a
    /// flight behavior feature, defined here so the wire never breaks again
    /// when they land.
    inline constexpr std::uint8_t RC_MODE_MANUAL = 0U;

    /// The stick commands target vertical velocity, centered = hold altitude.
    inline constexpr std::uint8_t RC_MODE_ALTITUDE_AUTO = 1U;

#pragma pack(push, 1)
    /// One whole run, scripted: what the plant must play and what makes it
    /// reproducible. A single layout for every scenario keeps the framing
    /// trivial; fields unused by the scenario are ignored. Vectors use the
    /// drone frame convention of the protocol (body x forward, y left, z up;
    /// world z up).
    ///
    /// A scenario is a header-less block, so it can ride inside another
    /// packet: it travels in every lockstep reply (SimActuatorPacket) as
    /// well as inside SimScenarioPacket. Every scenario opens with a reset,
    /// and everything after that reset tick is scheduled by the plant on its
    /// own tick grid - which is why the absolute tick a scenario arrives on
    /// never has to be agreed upon.
    struct SimScenario
    {
        std::uint8_t sequence;                    ///< 0 = no scenario; senders count 1..255
                                                  ///< and wrap, a receiver plays a block
                                                  ///< once per change of this byte
        std::uint8_t scenario;                    ///< one of the SIM_SCENARIO_* values
        std::uint64_t seed;                       ///< seeds every generator of the run
        std::uint32_t throwDelayUs;               ///< reset tick to throw or grab [us]
        std::uint32_t hashWindowUs;               ///< simulated time the flight process
                                                  ///< hashes from the reset, 0 = its
                                                  ///< default. Addressed to the flight
                                                  ///< process; the plant ignores it.
        std::array<float, 3> velocityMps;         ///< (HAND_)THROW: release velocity, world [m/s]
        std::array<float, 3> angularVelocityRadS; ///< (HAND_)THROW: release spin, body [rad/s]
        float heldSeconds;                        ///< HAND_THROW: hold before the swing [s]
        float heldTiltRad;                        ///< HAND_THROW: held tilt from level [rad]
        float heldAzimuthRad;                     ///< HAND_THROW: world azimuth of the tilt [rad]
        float swingSeconds;                       ///< HAND_THROW: swing duration, 0 = hold only [s]
    };

    /// One scenario addressed to a flight process, which forwards the block
    /// to the plant on its next lockstep reply.
    struct SimScenarioPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::SIM_SCENARIO
        SimScenario scenario; ///< the run to play
    };
#pragma pack(pop)

    /// sequence (1) + scenario (1) + seed (8) + throw delay (4) + hash window
    /// (4) + velocity (12) + angular velocity (12) + held (4) + held tilt (4)
    /// + held azimuth (4) + swing (4).
    inline constexpr std::size_t SIM_SCENARIO_SIZE = 58U;

    /// version (1) + type (1) + scenario block (58).
    inline constexpr std::size_t SIM_SCENARIO_PACKET_SIZE = 60U;

    static_assert(sizeof(SimScenario) == SIM_SCENARIO_SIZE, "wire layout must be packed");
    static_assert(sizeof(SimScenarioPacket) == SIM_SCENARIO_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<SimScenario>);
    static_assert(std::is_trivially_copyable_v<SimScenarioPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(SimScenario, sequence) == 0U);
    static_assert(offsetof(SimScenario, scenario) == 1U);
    static_assert(offsetof(SimScenario, seed) == 2U);
    static_assert(offsetof(SimScenario, throwDelayUs) == 10U);
    static_assert(offsetof(SimScenario, hashWindowUs) == 14U);
    static_assert(offsetof(SimScenario, velocityMps) == 18U);
    static_assert(offsetof(SimScenario, angularVelocityRadS) == 30U);
    static_assert(offsetof(SimScenario, heldSeconds) == 42U);
    static_assert(offsetof(SimScenario, heldTiltRad) == 46U);
    static_assert(offsetof(SimScenario, heldAzimuthRad) == 50U);
    static_assert(offsetof(SimScenario, swingSeconds) == 54U);
    static_assert(offsetof(SimScenarioPacket, version) == 0U);
    static_assert(offsetof(SimScenarioPacket, type) == 1U);
    static_assert(offsetof(SimScenarioPacket, scenario) == 2U);
    // NOLINTEND(readability-magic-numbers)

#pragma pack(push, 1)
    /// Pilot state for the firmware uplink. Streamed periodically (10 Hz),
    /// never fired once: the firmware treats a silent link as a kill
    /// (fail-safe), so holding a state means repeating it.
    struct RcCommandPacket
    {
        std::uint8_t version;    ///< = PROTOCOL_VERSION
        std::uint8_t type;       ///< = PacketType::RC_COMMAND
        std::uint8_t killSwitch; ///< 1 = engaged (motors cut)
        std::uint8_t armSwitch;  ///< 1 = armed for an autonomous throw flight
        std::uint8_t mode;       ///< one of the RC_MODE_* values
        float throttle;          ///< normalized [0, 1]
    };
#pragma pack(pop)

    /// version (1) + type (1) + kill switch (1) + arm switch (1) + mode (1)
    /// + throttle (4).
    inline constexpr std::size_t RC_COMMAND_PACKET_SIZE = 9U;

    static_assert(sizeof(RcCommandPacket) == RC_COMMAND_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<RcCommandPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(RcCommandPacket, killSwitch) == 2U);
    static_assert(offsetof(RcCommandPacket, armSwitch) == 3U);
    static_assert(offsetof(RcCommandPacket, mode) == 4U);
    static_assert(offsetof(RcCommandPacket, throttle) == 5U);
    // NOLINTEND(readability-magic-numbers)

    /// Third byte of RebootCommandPacket: the magic makes a stray line
    /// glitch decoding into a reboot implausible.
    inline constexpr std::uint8_t BOARD_REBOOT_MAGIC = 0xB7U;

#pragma pack(push, 1)
    /// Reboots the flight controller (NVIC system reset), the serial
    /// counterpart of the simulator reset: one bench gesture restarts
    /// whichever world is listening. Telemetry, blackbox bytes in flight
    /// and the RC state are all lost, which is the point.
    struct RebootCommandPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::REBOOT_COMMAND
        std::uint8_t magic;   ///< = BOARD_REBOOT_MAGIC
    };
#pragma pack(pop)

    /// version (1) + type (1) + magic (1).
    inline constexpr std::size_t REBOOT_COMMAND_PACKET_SIZE = 3U;

    static_assert(sizeof(RebootCommandPacket) == REBOOT_COMMAND_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<RebootCommandPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(RebootCommandPacket, magic) == 2U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
