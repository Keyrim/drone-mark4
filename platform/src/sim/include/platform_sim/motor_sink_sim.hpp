#pragma once

/// @file
/// @brief UDP motor sink for the sim variant.

#include <cstdint>

#include "platform/motor_sink.hpp"
#include "platform_sim/udp_link.hpp"
#include "protocol/commands.hpp"

namespace mark4
{
    /// Sends every actuator frame back to the simulator, on the address the
    /// last sensor packet came from, and keeps the last frame for reporting.
    ///
    /// It also carries the scenario block to the plant. The block is latched,
    /// not queued: every reply repeats it, and the plant plays it once per
    /// change of its sequence byte. The lockstep handshake is what makes that
    /// reliable - the plant resends its sensor packet until the reply for
    /// that exact tick arrives, so the block cannot be lost without the tick
    /// being lost with it.
    class MotorSinkSim final : public AbsMotorSink
    {
      public:
        /// @param link bound sim link the actuator packets are sent on
        explicit MotorSinkSim(UdpLink &link)
            : m_link(link)
        {
        }

        /// @brief Records the frame and sends it as an actuator packet. Best
        ///        effort: a send failure is logged by the link, not reported.
        /// @param frame actuator frame to output
        void push(const mark4::ActuatorFrame &frame) override;

        /// @brief Latches the scenario block repeated by every later reply.
        ///        Replacing it before the plant has seen it simply means the
        ///        plant never sees the replaced one; the sender owns that
        ///        pacing, not this sink.
        /// @param scenario block to carry, sequence 0 to carry none
        void attachScenario(const mark4::SimScenario &scenario)
        {
            m_scenario = scenario;
        }

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
        UdpLink &m_link;                 ///< sim link, owned by the composition root
        mark4::ActuatorFrame m_last;     ///< last frame pushed
        mark4::SimScenario m_scenario{}; ///< block repeated by every reply
        std::uint32_t m_pushCount = 0U;  ///< frames pushed since construction
    };
} // namespace mark4
