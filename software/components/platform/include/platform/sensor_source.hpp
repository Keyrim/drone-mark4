#pragma once

/// @file
/// @brief Blocking sensor input, the single wait point of the system.

#include <cstdint>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Outcome of one wait on the sensor source.
    enum class FrameWait : std::uint8_t
    {
        FRAME = 0U,     ///< a frame was produced
        TIMEOUT = 1U,   ///< transient: nothing arrived within the source's own
                        ///< wait window; retrying may well succeed
        EXHAUSTED = 2U, ///< permanent: the source will never produce again
                        ///< (link closed, shutdown)
    };

    /// Produces sensor frames, blocking until one is available.
    class AbsSensorSource
    {
      public:
        virtual ~AbsSensorSource() = default;

        /// @brief Blocks until the next frame is available or the source's
        ///        wait window expires. The retry-or-give-up policy belongs to
        ///        the caller: TIMEOUT says nothing about the future and a
        ///        caller may retry forever, EXHAUSTED is final and retrying
        ///        is a programming error.
        /// @param[out] frameOut filled frame, valid only when FRAME is returned
        /// @return FRAME, TIMEOUT or EXHAUSTED
        virtual FrameWait waitFrame(mark4::SensorFrame &frameOut) = 0;
    };
} // namespace mark4
