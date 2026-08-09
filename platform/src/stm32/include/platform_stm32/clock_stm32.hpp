#pragma once

/// @file
/// @brief Monotonic microsecond clock on a hardware timer.

#include <cstdint>

#include "platform/clock.hpp"

namespace mark4
{
    /// TIM2 free-running at 1 MHz. The 32-bit counter wraps every ~71
    /// minutes; nowUs() extends it to 64 bits with a read-modify-write on
    /// the wrap counter. Two hard constraints follow, both honored by the
    /// one flight loop and worth restating before any second caller shows
    /// up: nowUs() must be called at least once per counter wrap (the
    /// 500 Hz loop calls it every frame), and it must only ever be called
    /// from a single context - a call from an interrupt could interleave
    /// with one from the loop and double-count a wrap.
    class ClockStm32 final : public AbsClock
    {
      public:
        /// @brief Starts the timer. Assumes the 84 MHz APB1 timer clock
        ///        set by initSystemClock().
        void init();

        /// @return microseconds since init(). Single-context only, and at
        ///         least once per ~71 min wrap; see the class contract.
        std::uint64_t nowUs() override;

      private:
        std::uint32_t m_lastCount = 0U; ///< last raw counter read, wrap detection
        std::uint32_t m_wraps = 0U;     ///< completed 32-bit counter periods
    };
} // namespace mark4
