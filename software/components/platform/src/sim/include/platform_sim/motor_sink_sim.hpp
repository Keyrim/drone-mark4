#pragma once

/// @file
/// @brief UDP motor sink for the sim variant.

#include <cstdint>

#include "platform/motor_sink.hpp"
#include "platform_sim/udp_socket.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Sends every actuator frame back to the simulator as a SimActuator
    /// envelope, on the address the last sensor message came from, and keeps
    /// the last frame for reporting.
    ///
    /// It also carries scenarios to the plant, each as its own SimScenario
    /// envelope on the same link. A scenario handed over before the plant
    /// has spoken is kept until the first actuator frame goes out, since
    /// only then is there an address to send it to.
    class MotorSinkSim final : public AbsMotorSink
    {
      public:
        /// @param link bound sim link the actuator envelopes are sent on
        explicit MotorSinkSim(UdpSocket &link)
            : m_link(link)
        {
        }

        /// @brief Records the frame and sends it as a SimActuator envelope,
        ///        preceded by a pending scenario when one waits. Best effort:
        ///        a send failure is logged by the link, not reported.
        /// @param frame actuator frame to output
        void push(const mark4::ActuatorFrame &frame) override;

        /// @brief Sends one scenario to the plant, now when its address is
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
        /// @return true when the datagram was handed to the stack
        bool emitScenario(const mark4_SimScenario &scenario);

        UdpSocket &m_link;           ///< sim link, owned by the composition root
        mark4::ActuatorFrame m_last; ///< last frame pushed
        mark4_SimScenario m_pendingScenario = mark4_SimScenario_init_zero; ///< waiting for a peer
        bool m_scenarioPending = false; ///< m_pendingScenario is still to send
        std::uint32_t m_pushCount = 0U; ///< frames pushed since construction
    };
} // namespace mark4
