#pragma once

/// @file
/// @brief Board LED policy on LED1 alone (LED2 is dead on this unit, see
///        docs/bring-up.md): one pattern at a time. Safety logic: solid
///        means the motors are commanded or imminent, a slow blink means
///        they may start on their own, a fast blink means a latched
///        incident, and short flashes mean inert - their count is the
///        detail (one: healthy, two: a service reports failures, three:
///        kill switch or RC fail-safe engaged).

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
    /// @param killEngaged true while the kill switch is engaged, whether
    ///        by the pilot or by the RC fail-safe
    /// @param degraded true while a service reports failures (I2C errors,
    ///        frame overruns, dropped packets); doubles the idle flash
    /// @param frameIndex frames since boot, drives the pattern cycle
    void updateStatusLeds(FlightPhase phase,
                          bool killEngaged,
                          bool degraded,
                          std::uint32_t frameIndex);
} // namespace mark4
