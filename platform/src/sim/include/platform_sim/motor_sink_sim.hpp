#pragma once

/// @file
/// @brief Recording motor sink for the sim variant.

#include <cstdint>

#include "platform/motor_sink.hpp"

namespace mark4
{
    /// Records the last actuator frame (will feed the simulator back later).
    class MotorSinkSim final : public AbsMotorSink
    {
      public:
        void push(const mark4::ActuatorFrame &frame) override;

        /// @return last pushed frame
        [[nodiscard]] const mark4::ActuatorFrame &last() const
        {
            return m_last;
        }

        /// @return number of frames pushed since construction
        [[nodiscard]] std::uint32_t pushCount() const
        {
            return m_pushCount;
        }

      private:
        mark4::ActuatorFrame m_last;
        std::uint32_t m_pushCount = 0U;
    };
} // namespace mark4
