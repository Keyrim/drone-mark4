#pragma once

/// @file
/// @brief Decimated Status publishing, shared by every composition.

#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "platform_common/envelope_io.hpp"
#include "platform_common/status_packer.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// Packs one Status report every DECIMATION frames and broadcasts it.
    /// Owns the frame counter, so every composition decimates the same
    /// stream the same way instead of each keeping a drifting copy of the
    /// counter and the factor.
    class StatusPublisher
    {
      public:
        /// Frames per Status message: a 500 Hz loop reports at 50 Hz.
        static constexpr std::uint32_t DECIMATION = 10U;

        /// @param transport transport the reports are broadcast on
        explicit StatusPublisher(Transport &transport)
            : m_transport(transport)
        {
        }

        /// @brief Counts one frame and publishes every DECIMATION-th.
        /// @param frame sensor frame of this step
        /// @param actuators actuator outputs of this step
        /// @param core flight core the estimates are read from
        /// @param rcLinkOk true when the RC fail-safe is not active for this
        ///        frame, so a pilot's device can tell it is being heard
        /// @param truth the plant's exact state at this frame, nullptr when
        ///        the composition has none (a real board)
        void publish(const SensorFrame &frame,
                     const ActuatorFrame &actuators,
                     const FlightCore &core,
                     bool rcLinkOk,
                     const mark4_PlantTruth *truth = nullptr)
        {
            ++m_frameCount;
            if (m_frameCount % DECIMATION != 0U)
            {
                return;
            }
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_status_tag;
            packStatus(frame, actuators, core, rcLinkOk, envelope.body.status);
            if (truth != nullptr)
            {
                envelope.body.status.has_truth = true;
                envelope.body.status.truth = *truth;
            }
            static_cast<void>(sendEnvelope(m_transport, BROADCAST_NODE, envelope));
        }

      private:
        Transport &m_transport;          ///< output, not owned
        std::uint32_t m_frameCount = 0U; ///< frames seen since construction
    };
} // namespace mark4
