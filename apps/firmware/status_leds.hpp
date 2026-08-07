#pragma once

/// @file
/// @brief Board LED policy on LED1 alone (LED2 is dead on this unit, see
///        docs/bring-up.md): one pattern at a time, the flight state has
///        priority and health only shows while idle. Safety logic: solid
///        means the motors are commanded or imminent, a slow blink means
///        they may start on their own, short flashes mean inert, a fast
///        blink means a latched incident.

#include <cstdint>

#include "flight_core/flight_core.hpp"

namespace mark4
{
    /// LED patterns cycle over one second, split in 20 slots of 50 ms:
    /// one bit per slot, least significant bit first.
    inline constexpr std::uint32_t LED_PATTERN_SLOTS = 20U;

    /// Slot duration in flight loop frames: 50 ms at 500 Hz.
    inline constexpr std::uint32_t LED_FRAMES_PER_SLOT = 25U;

    /// @brief Drives the status LED for one frame.
    /// @param phase current phase of the flight state machine
    /// @param degraded true while a service reports failures (I2C errors,
    ///        frame overruns, dropped packets); doubles the idle flash
    /// @param frameIndex frames since boot, drives the pattern cycle
    void updateStatusLeds(FlightPhase phase, bool degraded, std::uint32_t frameIndex);
} // namespace mark4
