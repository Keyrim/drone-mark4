#include "platform_sim/clock_sim.hpp"

namespace mark4
{
    std::uint64_t ClockSim::nowUs()
    {
        const auto elapsed = std::chrono::steady_clock::now() - m_epoch;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
} // namespace mark4
