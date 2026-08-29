#pragma once

/// @file
/// @brief Motor sink of the sim variant: SimActuator envelopes to the plant.

#include <cstdint>

#include "platform/motor_sink.hpp"
#include "platform_sim/plant_link.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Sends every actuator frame to the plant as a SimActuator envelope,
    /// unicast to the node the plant link holds, and keeps the last frame
    /// for reporting.
    ///
    /// It also carries scenarios to the plant, each as its own SimScenario
    /// envelope on the same link. A scenario handed over before a plant is
    /// known is kept until the first actuator frame goes out, since only
    /// then is there a node to send it to.
    class MotorSinkSim final : public AbsMotorSink
    {
      public:
        /// @param link plant link the actuator envelopes are sent on
        explicit MotorSinkSim(PlantLink &link)
            : m_link(link)
        {
        }

        /// @brief Records the frame and sends it as a SimActuator envelope,
        ///        preceded by a pending scenario when one waits. Best effort:
        ///        a send failure is counted by the transport, not reported.
        /// @param frame actuator frame to output
        void push(const mark4::ActuatorFrame &frame) override;

        /// @brief Sends one scenario to the plant, now when its node is
        ///        known, otherwise with the next actuator frame. The plant
        ///        plays a scenario once per change of its sequence.
        /// @param scenario run to play
        void sendScenario(const mark4_SimScenario &scenario);

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
        /// @brief Encodes and sends one scenario envelope.
        /// @param scenario run to play
        /// @return true when the frame left
        bool emitScenario(const mark4_SimScenario &scenario);

        PlantLink &m_link;           ///< plant link, owned by the composition root
        mark4::ActuatorFrame m_last; ///< last frame pushed
        mark4_SimScenario m_pendingScenario = mark4_SimScenario_init_zero; ///< waiting for a plant
        bool m_scenarioPending = false; ///< m_pendingScenario is still to send
        std::uint32_t m_pushCount = 0U; ///< frames pushed since construction
    };
} // namespace mark4
