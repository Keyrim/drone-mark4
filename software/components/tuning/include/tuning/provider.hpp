#pragma once

/// @file
/// @brief The parameter table of the flight core on the wire: the adapter
///        between the tuning messages of the schema and the registry the
///        core owns. flight-core never includes a wire header, so this is
///        where the two vocabularies meet and where they are pinned to each
///        other.
///
/// Timing contract. onMessage() runs inside the messenger poll, which every
/// composition runs before step(), so a value written by a request is in
/// effect for the whole of the next step and never changes one halfway
/// through. The core is single-threaded and so is this: no locking, no queue.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/flight_core.hpp"
#include "flight_core/tuning_table.hpp"
#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"

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

    /// Answers the tuning requests the messenger hands it, to the node that
    /// asked: the acknowledgement of a set or a get, and one page of the
    /// table per list request.
    class TuningProvider final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 3> TAGS = {mark4_Envelope_tuning_set_tag,
                                                          mark4_Envelope_tuning_get_tag,
                                                          mark4_Envelope_tuning_list_request_tag};

        /// Descriptions per TuningInfos page, from the wire bound. One page
        /// per request: the consumer paces the walk, so a table dump never
        /// bursts ahead of the telemetry sharing the same link.
        static constexpr std::size_t INFOS_PER_PAGE =
            sizeof(mark4_TuningInfos::infos) / sizeof(mark4_TuningInfos::infos[0]);

        /// @param messenger messenger the requests come from and the answers
        ///        leave by; must outlive the provider
        /// @param core flight core owning the parameter registry
        TuningProvider(Messenger &messenger, FlightCore &core)
            : AbsMessageHandler(messenger, TAGS),
              m_core(core)
        {
        }

        /// @brief Answers one tuning request, to the node that sent it.
        /// @param src node it came from: where the answer goes
        /// @param envelope decoded message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        nothing here is timed
        /// @return true when the message was answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            static_cast<void>(nowUs);
            switch (envelope.which_body)
            {
                case mark4_Envelope_tuning_set_tag:
                    ++m_requestCount;
                    sendAck(src,
                            envelope.body.tuning_set.id,
                            m_core.setParam(static_cast<std::uint16_t>(envelope.body.tuning_set.id),
                                            envelope.body.tuning_set.value));
                    return true;
                case mark4_Envelope_tuning_get_tag: {
                    ++m_requestCount;
                    float value = 0.0f;
                    sendAck(src,
                            envelope.body.tuning_get.id,
                            m_core.getParam(static_cast<std::uint16_t>(envelope.body.tuning_get.id),
                                            value));
                    return true;
                }
                case mark4_Envelope_tuning_list_request_tag:
                    ++m_requestCount;
                    sendPage(src, envelope.body.tuning_list_request.cursor);
                    return true;
                default:
                    return false;
            }
        }

        /// @return tuning requests understood since construction
        [[nodiscard]] std::uint32_t requestCount() const
        {
            return m_requestCount;
        }

        /// @return answers emitted since construction, pages included
        [[nodiscard]] std::uint32_t answerCount() const
        {
            return m_answerCount;
        }

      private:
        /// @return the logging module of the provider. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TUNING_PROVIDER, "tuning/provider"};
            return MODULE;
        }

        /// @brief Sends one acknowledgement carrying the value in effect.
        /// @param dst node that sent the request
        /// @param id parameter id echoed from the request
        /// @param status outcome of the request
        void sendAck(std::uint32_t dst, std::uint32_t id, TuningStatus status)
        {
            // The value that travels is always the live one, whatever the
            // outcome: a refused write is answered with what is still flying,
            // so a ground station never has to guess what it ended up with.
            float value = 0.0f;
            static_cast<void>(m_core.getParam(static_cast<std::uint16_t>(id), value));
            if (status != TuningStatus::OK)
            {
                Module().warn("%08lx asked for parameter %lu: refused (status %u)",
                              static_cast<unsigned long>(dst),
                              static_cast<unsigned long>(id),
                              static_cast<unsigned>(status));
            }

            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_ack_tag;
            envelope.body.tuning_ack.id = id;
            envelope.body.tuning_ack.value = value;
            envelope.body.tuning_ack.status = static_cast<mark4_TuningStatus>(status);
            send(dst, envelope);
        }

        /// @brief Sends one page of the parameter table.
        /// @param dst node that asked: where the page goes
        /// @param cursor first table index to describe; past the end sends an
        ///        empty page carrying the total, which is how a requester
        ///        learns it asked too far
        void sendPage(std::uint32_t dst, std::uint32_t cursor)
        {
            const std::size_t total = FlightCore::ParamCount();
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_infos_tag;
            mark4_TuningInfos &page = envelope.body.tuning_infos;
            page.total = static_cast<std::uint32_t>(total);
            page.cursor = cursor;
            // Clamped first, so the walk below is provably inside the table
            // whatever cursor the requester sent.
            const std::size_t first = cursor > total ? total : static_cast<std::size_t>(cursor);
            for (std::size_t index = first;
                 index < total && page.infos_count < INFOS_PER_PAGE;
                 ++index)
            {
                const TuningParam *const param = m_core.paramInfo(index);
                if (param == nullptr)
                {
                    break;
                }
                fillInfo(*param, page.infos[page.infos_count]);
                ++page.infos_count;
            }
            send(dst, envelope);
        }

        /// @brief Describes one parameter as the wire does.
        /// @param param entry to describe
        /// @param[out] infoOut message to fill
        static void fillInfo(const TuningParam &param, mark4_TuningInfo &infoOut)
        {
            infoOut.id = param.id;
            // The registry name is zero-padded and a full-length one carries
            // no terminator; the wire field has one byte more for it.
            std::memcpy(infoOut.name, param.name.data(), TuningParam::NAME_SIZE);
            infoOut.name[TuningParam::NAME_SIZE] = '\0';
            infoOut.value = param.value;
            infoOut.min_value = param.minValue;
            infoOut.max_value = param.maxValue;
            infoOut.armed_change = param.armedChange;
        }

        /// @brief Sends one answer to the node that asked, as a request of
        ///        this provider's: an answer that matters is acknowledged
        ///        like everything that has to arrive.
        /// @param dst node the answer goes to
        /// @param envelope answer to send
        void send(std::uint32_t dst, mark4_Envelope &envelope)
        {
            if (request(dst, envelope))
            {
                ++m_answerCount;
            }
        }

        FlightCore &m_core;                ///< registry owner, not owned
        std::uint32_t m_requestCount = 0U; ///< tuning requests understood
        std::uint32_t m_answerCount = 0U;  ///< acks and pages emitted
    };
} // namespace mark4
