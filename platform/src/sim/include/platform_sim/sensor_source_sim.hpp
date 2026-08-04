#pragma once

/// @file
/// @brief Self-generating sensor source for the sim variant.

#include <cstdint>

#include "platform/clock.hpp"
#include "platform/sensor_source.hpp"

namespace mark4
{
    /// Generates plausible frames internally at a fixed rate (sinusoidal gyro,
    /// gravity at rest), timestamped by the injected clock. Exhausts itself
    /// after `maxFrames` so the main loop terminates cleanly. No network yet.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        SensorSourceSim(AbsClock &clock, std::uint64_t periodUs, std::uint32_t maxFrames)
            : m_clock(clock),
              m_periodUs(periodUs),
              m_maxFrames(maxFrames)
        {
        }

        bool waitFrame(mark4::SensorFrame &frameOut) override;

      private:
        AbsClock &m_clock;
        std::uint64_t m_periodUs;
        std::uint32_t m_maxFrames;
        std::uint32_t m_produced = 0U;
    };
} // namespace mark4
