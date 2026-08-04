#pragma once

/// @file
/// @brief Flight core entry point.

#include <cstdint>

#include "flight_core/types.hpp"

namespace mark4
{
    /// Synchronous, single-threaded flight core, paced by data arrival
    /// (never by time). Pure: no allocation, no waiting, no clock access.
    class FlightCore
    {
      public:
        /// @brief Runs one control step. The kill switch is honored first.
        /// @param sensors latest sensor frame
        /// @param[out] actuators motor commands computed for this step
        void step(const SensorFrame &sensors, ActuatorFrame &actuators);

        /// @return number of steps executed since construction
        [[nodiscard]] std::uint32_t stepCount() const
        {
            return m_stepCount;
        }

      private:
        std::uint32_t m_stepCount = 0U;
    };
} // namespace mark4
