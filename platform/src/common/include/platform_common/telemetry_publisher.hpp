#pragma once

/// @file
/// @brief Decimated telemetry publishing, shared by every composition.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/telemetry.hpp"
#include "flight_core/types.hpp"
#include "platform/telemetry_sender.hpp"

namespace mark4
{
    /// Packs one telemetry snapshot every DECIMATION frames and hands it to
    /// the sender. Owns the frame counter, so every composition decimates the
    /// same stream the same way instead of each keeping a drifting copy of
    /// the counter and the factor.
    class TelemetryPublisher
    {
      public:
        /// Frames per telemetry packet: a 500 Hz loop telemeters at 50 Hz.
        static constexpr std::uint32_t DECIMATION = 10U;

        /// @param sender telemetry output the packed snapshots go to
        explicit TelemetryPublisher(AbsTelemetrySender &sender)
            : m_sender(sender)
        {
        }

        /// @brief Counts one frame and publishes every DECIMATION-th.
        /// @param frame sensor frame of this step
        /// @param actuators actuator outputs of this step
        /// @param core flight core the estimates are read from
        void publish(const SensorFrame &frame, const ActuatorFrame &actuators,
                     const FlightCore &core)
        {
            ++m_frameCount;
            if (m_frameCount % DECIMATION != 0U)
            {
                return;
            }
            const auto wire = packTelemetry(frame, actuators, core);
            m_sender.send(wire.data(), wire.size());
        }

      private:
        AbsTelemetrySender &m_sender;    ///< output link, not owned
        std::uint32_t m_frameCount = 0U; ///< frames seen since construction
    };
} // namespace mark4
