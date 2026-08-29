/// @file
/// @brief Link health helpers.

#include "hub/stream_health.hpp"

namespace mark4
{
    double linkLossRate(const LinkHealth &link)
    {
        const double expected = static_cast<double>(link.received) + static_cast<double>(link.lost);
        if (expected <= 0.0)
        {
            return 0.0;
        }
        return static_cast<double>(link.lost) / expected;
    }
} // namespace mark4
