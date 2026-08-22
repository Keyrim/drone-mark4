#pragma once

/// @file
/// @brief Non-blocking motor command output.

#include "flight_core/types.hpp"

namespace mark4
{
    /// Consumes actuator frames (ESC on target, simulator response elsewhere).
    class AbsMotorSink
    {
      public:
        virtual ~AbsMotorSink() = default;

        /// @brief Pushes motor commands. Must not block.
        /// @param frame actuator frame to output
        virtual void push(const mark4::ActuatorFrame &frame) = 0;
    };
} // namespace mark4
