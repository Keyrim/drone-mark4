#pragma once

/// @file
/// @brief Blocking sensor input, the single wait point of the system.

#include "flight_core/types.hpp"

namespace mark4
{
    /// Produces sensor frames, blocking until one is available.
    class AbsSensorSource
    {
      public:
        virtual ~AbsSensorSource() = default;

        /// @brief Blocks until the next frame is available.
        /// @param[out] frameOut filled frame, valid only when returning true
        /// @return true when a frame was produced, false when the source is
        ///         exhausted (end of replay, shutdown requested)
        virtual bool waitFrame(mark4::SensorFrame &frameOut) = 0;
    };
} // namespace mark4
