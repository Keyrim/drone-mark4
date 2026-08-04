#pragma once

/// @file
/// @brief Time service, internal to platform.

#include <cstdint>

namespace mark4
{
    /// Monotonic time source. Serves platform services between themselves
    /// (frame timestamping); never passed to the flight core.
    class AbsClock
    {
      public:
        virtual ~AbsClock() = default;

        /// @return microseconds since an arbitrary monotonic origin
        virtual std::uint64_t nowUs() = 0;
    };
} // namespace mark4
