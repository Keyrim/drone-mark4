#pragma once

/// @file
/// @brief The updater on the wire: the Ota* requests of `protocol/mark4.proto`
///        in, the updater's reply out, to the node that asked.

#include <array>
#include <cstdint>

#include "messaging/messenger.hpp"
#include "ota/updater.hpp"
#include "protocol/envelope.hpp"
#include "services/ota_gate.hpp"

namespace mark4
{
    /// The updater on the wire: one Ota* request in, at most one reply out, to
    /// the requester. Pure glue between the messenger and the OtaUpdater.
    class OtaService final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes: every request the updater answers.
        static constexpr std::array<pb_size_t, 6> TAGS = {mark4_Envelope_ota_status_request_tag,
                                                          mark4_Envelope_ota_begin_tag,
                                                          mark4_Envelope_ota_chunk_tag,
                                                          mark4_Envelope_ota_finish_tag,
                                                          mark4_Envelope_ota_revert_tag,
                                                          mark4_Envelope_ota_abort_tag};

        /// @param messenger messenger the requests come from and the replies
        ///        leave by; must outlive the service
        /// @param updater the session state machine served; must outlive the
        ///        service
        /// @param gate what the node answers about itself: the arming state
        ///        the updater refuses to start a session in, and the pack
        ///        voltage floor; must outlive the service
        OtaService(Messenger &messenger, OtaUpdater &updater, const AbsOtaGate &gate)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger),
              m_updater(updater),
              m_gate(gate)
        {
        }

        /// @brief Hands one request to the updater and sends its reply, when
        ///        there is one, to the requester.
        /// @param src node the request came from: where the reply goes
        /// @param envelope decoded request
        /// @param nowUs instant of the poll that delivered it [us], the
        ///        updater's session timeout base
        /// @return true when the updater consumed the request, whatever it
        ///         answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            OtaUpdater::Inputs inputs;
            inputs.armed = m_gate.armed();
            inputs.voltageOk = m_gate.voltageOk();
            inputs.nowUs = nowUs;

            mark4_Envelope reply;
            const bool consumed = m_updater.handle(envelope, inputs, reply);
            if (reply.which_body != 0U)
            {
                static_cast<void>(m_messenger.send(src, reply));
            }
            if (consumed)
            {
                ++m_consumed;
            }
            return consumed;
        }

        /// @return requests the updater consumed since construction; a composition
        ///         that watches it re-reads what a consumed request may have changed
        [[nodiscard]] std::uint32_t consumed() const
        {
            return m_consumed;
        }

      private:
        Messenger &m_messenger;        ///< reply route, not owned
        OtaUpdater &m_updater;         ///< the session served, not owned
        const AbsOtaGate &m_gate;      ///< what the node answers per request, not owned
        std::uint32_t m_consumed = 0U; ///< requests the updater consumed
    };
} // namespace mark4
