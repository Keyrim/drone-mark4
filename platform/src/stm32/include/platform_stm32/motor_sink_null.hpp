#pragma once

/// @file
/// @brief Motor sink for a bench board with no ESC wired.

#include "platform/motor_sink.hpp"

namespace mark4
{
    /// Discards the commands, keeping the last frame readable so the app
    /// can report what the core WOULD drive. Replaced by the DShot sink
    /// once motors exist.
    class MotorSinkNull final : public AbsMotorSink
    {
      public:
        /// @brief Records the frame, drives nothing.
        /// @param frame actuator frame to output
        void push(const mark4::ActuatorFrame &frame) override
        {
            m_last = frame;
        }

        /// @return last frame pushed by the core
        [[nodiscard]] const mark4::ActuatorFrame &last() const
        {
            return m_last;
        }

      private:
        mark4::ActuatorFrame m_last; ///< last commands, for status logs
    };
} // namespace mark4
