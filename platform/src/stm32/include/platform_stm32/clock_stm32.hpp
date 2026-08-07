#pragma once

/// @file
/// @brief Monotonic microsecond clock on a hardware timer.

#include <cstdint>

#include "platform/clock.hpp"

namespace mark4
{
    /// TIM2 free-running at 1 MHz. The 32-bit counter wraps every ~71
    /// minutes; nowUs() extends it to 64 bits, which holds as long as it
    /// is called more than once per wrap (the sensor loop calls it every
    /// frame).
    class ClockStm32 final : public AbsClock
    {
      public:
        /// @brief Starts the timer. Assumes the 84 MHz APB1 timer clock
        ///        set by initSystemClock().
        void init();

        /// @return microseconds since init()
        std::uint64_t nowUs() override;

      private:
        std::uint32_t m_lastCount = 0U; ///< last raw counter read, wrap detection
        std::uint32_t m_wraps = 0U;     ///< completed 32-bit counter periods
    };
} // namespace mark4
