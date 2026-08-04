#pragma once

/// @file
/// @brief Monotonic clock for the sim variant.

#include <chrono>
#include <cstdint>

#include "platform/clock.hpp"

namespace mark4
{
    /// Monotonic clock started at construction.
    class ClockSim final : public AbsClock
    {
      public:
        ClockSim()
            : m_epoch(std::chrono::steady_clock::now())
        {
        }

        std::uint64_t nowUs() override;

      private:
        std::chrono::steady_clock::time_point m_epoch;
    };
} // namespace mark4
