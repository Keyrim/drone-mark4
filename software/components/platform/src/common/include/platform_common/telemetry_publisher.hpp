#pragma once

/// @file
/// @brief Decimated telemetry publishing, shared by every composition.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "platform_common/envelope_io.hpp"
#include "platform_common/telemetry_packer.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// Packs one telemetry snapshot every DECIMATION frames and broadcasts
    /// it. Owns the frame counter, so every composition decimates the same
    /// stream the same way instead of each keeping a drifting copy of the
    /// counter and the factor.
    class TelemetryPublisher
    {
      public:
        /// Frames per telemetry message: a 500 Hz loop telemeters at 50 Hz.
        static constexpr std::uint32_t DECIMATION = 10U;

        /// @param transport transport the packed snapshots are broadcast on
        explicit TelemetryPublisher(Transport &transport)
            : m_transport(transport)
        {
        }

        /// @brief Counts one frame and publishes every DECIMATION-th.
        /// @param frame sensor frame of this step
        /// @param actuators actuator outputs of this step
        /// @param core flight core the estimates are read from
        /// @param truth the plant's exact state at this frame, nullptr when
        ///        the composition has none (a real board)
        void publish(const SensorFrame &frame,
                     const ActuatorFrame &actuators,
                     const FlightCore &core,
                     const mark4_PlantTruth *truth = nullptr)
        {
            ++m_frameCount;
            if (m_frameCount % DECIMATION != 0U)
            {
                return;
            }
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_telemetry_tag;
            packTelemetry(frame, actuators, core, envelope.body.telemetry);
            if (truth != nullptr)
            {
                envelope.body.telemetry.has_truth = true;
                envelope.body.telemetry.truth = *truth;
            }
            static_cast<void>(sendEnvelope(m_transport, BROADCAST_NODE, envelope));
        }

      private:
        Transport &m_transport;          ///< output, not owned
        std::uint32_t m_frameCount = 0U; ///< frames seen since construction
    };
} // namespace mark4
