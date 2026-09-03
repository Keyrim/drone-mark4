#pragma once

/// @file
/// @brief Tuning request dispatch, shared by every composition: the adapter
///        between the tuning messages of the wire and the parameter registry
///        the flight core owns. flight-core never includes a wire header, so
///        this is where the two vocabularies meet and where they are pinned
///        to each other.
///
/// Timing contract. handle() is called from the command drain loop, which
/// runs before step() in every composition, so a value written by a request
/// is in effect for the whole of the next step and never changes one halfway
/// through. The core is single-threaded and so is this: no locking, no queue.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/flight_core.hpp"
#include "flight_core/tuning_table.hpp"
#include "platform_common/envelope_io.hpp"
#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    // The statuses and the name width are the whole contract between the
    // registry and the wire. Asserting them here, one by one, is what lets
    // the two definitions stay independent without ever drifting apart.
    static_assert(static_cast<int>(TuningStatus::OK) == mark4_TuningStatus_OK);
    static_assert(static_cast<int>(TuningStatus::UNKNOWN_ID) == mark4_TuningStatus_UNKNOWN_ID);
    static_assert(static_cast<int>(TuningStatus::OUT_OF_BOUNDS) ==
                  mark4_TuningStatus_OUT_OF_BOUNDS);
    static_assert(static_cast<int>(TuningStatus::LOCKED_WHILE_ARMED) ==
                  mark4_TuningStatus_LOCKED_WHILE_ARMED);
    static_assert(TuningParam::NAME_SIZE + 1U == sizeof(mark4_TuningInfo::name),
                  "the parameter name width must match the wire");

    /// Answers the tuning messages a composition hands it, as transport
    /// broadcasts like the rest of the ground-bound traffic: a tuned value
    /// is state of the drone, and every ground tool watching wants it.
    class TuningService
    {
      public:
        /// Parameter descriptions emitted per pump() call. One, deliberately:
        /// a full table is a dozen frames, and a board that answers a list
        /// request by dumping all of them at once floods a 921600 baud UART
        /// for milliseconds and starves the telemetry stream sharing it. One
        /// per flight frame spreads the same answer over a few tens of ms,
        /// which no ground station notices.
        static constexpr std::size_t INFOS_PER_PUMP = 1U;

        /// @param core flight core owning the parameter registry
        /// @param transport transport the answers are broadcast on
        TuningService(FlightCore &core, Transport &transport)
            : m_core(core),
              m_transport(transport)
        {
        }

        /// @brief Answers one message, when it is a tuning request.
        /// @param envelope decoded message
        /// @return true when the message was a tuning request and was
        ///         answered, false when it belongs to someone else
        bool handle(const mark4_Envelope &envelope)
        {
            switch (envelope.which_body)
            {
                case mark4_Envelope_tuning_set_tag:
                    ++m_requestCount;
                    sendAck(envelope.body.tuning_set.id,
                            m_core.setParam(static_cast<std::uint16_t>(envelope.body.tuning_set.id),
                                            envelope.body.tuning_set.value));
                    return true;
                case mark4_Envelope_tuning_get_tag: {
                    ++m_requestCount;
                    float value = 0.0f;
                    sendAck(envelope.body.tuning_get.id,
                            m_core.getParam(static_cast<std::uint16_t>(envelope.body.tuning_get.id),
                                            value));
                    return true;
                }
                case mark4_Envelope_tuning_list_tag:
                    ++m_requestCount;
                    // Only the cursor is latched: the descriptions themselves go
                    // out from pump(), one per call. A second list request
                    // restarts the walk wherever it asks, which is also how a
                    // ground station recovers from a lost frame.
                    m_listCursor = envelope.body.tuning_list.start_index;
                    m_listPending = true;
                    return true;
                default:
                    return false;
            }
        }

        /// @brief Emits at most INFOS_PER_PUMP parameter descriptions. Call
        ///        once per flight frame; does nothing when no list request is
        ///        being served.
        void pump()
        {
            for (std::size_t emitted = 0U; m_listPending && emitted < INFOS_PER_PUMP; ++emitted)
            {
                const TuningParam *param = m_core.paramInfo(m_listCursor);
                if (param == nullptr)
                {
                    m_listPending = false;
                    return;
                }
                sendInfo(m_listCursor, *param);
                ++m_listCursor;
            }
        }

        /// @return tuning requests understood since construction
        [[nodiscard]] std::uint32_t requestCount() const
        {
            return m_requestCount;
        }

        /// @return answers emitted since construction, infos included
        [[nodiscard]] std::uint32_t answerCount() const
        {
            return m_answerCount;
        }

      private:
        /// @brief Sends one acknowledgement carrying the value in effect.
        /// @param id parameter id echoed from the request
        /// @param status outcome of the request
        void sendAck(std::uint32_t id, TuningStatus status)
        {
            // The value that travels is always the live one, whatever the
            // outcome: a refused write is answered with what is still flying,
            // so a ground station never has to guess what it ended up with.
            float value = 0.0f;
            static_cast<void>(m_core.getParam(static_cast<std::uint16_t>(id), value));

            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_ack_tag;
            envelope.body.tuning_ack.id = id;
            envelope.body.tuning_ack.value = value;
            envelope.body.tuning_ack.status = static_cast<mark4_TuningStatus>(status);
            send(envelope);
        }

        /// @brief Sends one parameter description.
        /// @param index table index of the entry
        /// @param param entry to describe
        void sendInfo(std::size_t index, const TuningParam &param)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_info_tag;
            mark4_TuningInfo &info = envelope.body.tuning_info;
            info.index = static_cast<std::uint32_t>(index);
            info.count = static_cast<std::uint32_t>(FlightCore::ParamCount());
            info.id = param.id;
            // The registry name is zero-padded and a full-length one carries
            // no terminator; the wire field has one byte more for it.
            std::memcpy(info.name, param.name.data(), TuningParam::NAME_SIZE);
            info.name[TuningParam::NAME_SIZE] = '\0';
            info.value = param.value;
            info.min_value = param.minValue;
            info.max_value = param.maxValue;
            info.armed_change = param.armedChange;
            send(envelope);
        }

        /// @brief Encodes an answer and hands the bytes to the link.
        /// @param envelope answer to send
        void send(const mark4_Envelope &envelope)
        {
            if (sendEnvelope(m_transport, BROADCAST_NODE, envelope))
            {
                ++m_answerCount;
            }
        }

        FlightCore &m_core;                ///< registry owner, not owned
        Transport &m_transport;            ///< answer route, not owned
        std::size_t m_listCursor = 0U;     ///< next table index to describe
        bool m_listPending = false;        ///< a list request is still unrolling
        std::uint32_t m_requestCount = 0U; ///< tuning requests understood
        std::uint32_t m_answerCount = 0U;  ///< acks and infos emitted
    };
} // namespace mark4
