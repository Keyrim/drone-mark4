#pragma once

/// @file
/// @brief Tuning request dispatch, shared by every composition: the adapter
///        between the tuning packets of protocol/ and the parameter registry
///        the flight core owns. flight-core never includes a wire header, so
///        this is where the two vocabularies meet and where they are pinned
///        to each other.
///
/// Timing contract. handle() is called from the command drain loop, which
/// runs before step() in every composition, so a value written by a request
/// is in effect for the whole of the next step and never changes one halfway
/// through. The core is single-threaded and so is this: no locking, no queue.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/flight_core.hpp"
#include "flight_core/tuning_table.hpp"
#include "platform/telemetry_sender.hpp"
#include "protocol/header.hpp"
#include "protocol/tuning.hpp"

namespace mark4
{
    // The statuses and the name width are the whole contract between the
    // registry and the wire. Asserting them here, one by one, is what lets
    // the two definitions stay independent without ever drifting apart.
    static_assert(static_cast<std::uint8_t>(TuningStatus::OK) == TUNING_ACK_OK);
    static_assert(static_cast<std::uint8_t>(TuningStatus::UNKNOWN_ID) == TUNING_ACK_UNKNOWN_ID);
    static_assert(static_cast<std::uint8_t>(TuningStatus::OUT_OF_BOUNDS) ==
                  TUNING_ACK_OUT_OF_BOUNDS);
    static_assert(static_cast<std::uint8_t>(TuningStatus::LOCKED_WHILE_ARMED) ==
                  TUNING_ACK_LOCKED_WHILE_ARMED);
    static_assert(TuningParam::NAME_SIZE == TUNING_NAME_SIZE,
                  "the parameter name width must match the wire");

    /// Answers the tuning packets a composition hands it, on the telemetry
    /// link the rest of the ground-bound traffic already uses (UDP broadcast
    /// in the simulator, framed UART on the board). Consumers demultiplex by
    /// header, so an ack sharing the stream with telemetry costs nothing.
    ///
    /// Owns no buffer of its own beyond one packet: the caller passes the
    /// bytes it drained, and every answer is built and sent in the same call.
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
        /// @param sender link the answers go out on
        TuningService(FlightCore &core, AbsTelemetrySender &sender)
            : m_core(core),
              m_sender(sender)
        {
        }

        /// @brief Answers one packet, when it is a tuning request.
        /// @param data packet bytes
        /// @param size packet size in bytes
        /// @return true when the packet was a tuning request and was
        ///         answered, false when it belongs to someone else
        bool handle(const std::uint8_t *data, std::size_t size)
        {
            if (data == nullptr)
            {
                return false;
            }
            if (size == TUNING_SET_PACKET_SIZE && hasHeader(data, size, PacketType::TUNING_SET))
            {
                TuningSetPacket request{};
                std::memcpy(&request, data, sizeof(request));
                ++m_requestCount;
                sendAck(request.id, m_core.setParam(request.id, request.value));
                return true;
            }
            if (size == TUNING_GET_PACKET_SIZE && hasHeader(data, size, PacketType::TUNING_GET))
            {
                TuningGetPacket request{};
                std::memcpy(&request, data, sizeof(request));
                ++m_requestCount;
                float value = 0.0f;
                sendAck(request.id, m_core.getParam(request.id, value));
                return true;
            }
            if (size == TUNING_LIST_PACKET_SIZE && hasHeader(data, size, PacketType::TUNING_LIST))
            {
                TuningListPacket request{};
                std::memcpy(&request, data, sizeof(request));
                ++m_requestCount;
                // Only the cursor is latched: the descriptions themselves go
                // out from pump(), one per call. A second list request
                // restarts the walk wherever it asks, which is also how a
                // ground station recovers from a lost frame.
                m_listCursor = request.startIndex;
                m_listPending = true;
                return true;
            }
            return false;
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
        void sendAck(std::uint16_t id, TuningStatus status)
        {
            // The value that travels is always the live one, whatever the
            // outcome: a refused write is answered with what is still flying,
            // so a ground station never has to guess what it ended up with.
            float value = 0.0f;
            static_cast<void>(m_core.getParam(id, value));

            TuningAckPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::TUNING_ACK);
            packet.id = id;
            packet.value = value;
            packet.status = static_cast<std::uint8_t>(status);
            send(packet);
        }

        /// @brief Sends one parameter description.
        /// @param index table index of the entry
        /// @param param entry to describe
        void sendInfo(std::size_t index, const TuningParam &param)
        {
            TuningInfoPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::TUNING_INFO);
            packet.index = static_cast<std::uint16_t>(index);
            packet.count = static_cast<std::uint16_t>(FlightCore::ParamCount());
            packet.id = param.id;
            // Copied byte by byte rather than assigned: assigning would bind
            // a reference to a field of a byte-packed struct.
            std::memcpy(&packet.name, param.name.data(), TUNING_NAME_SIZE);
            packet.value = param.value;
            packet.minValue = param.minValue;
            packet.maxValue = param.maxValue;
            packet.flags = param.armedChange ? TUNING_FLAG_ARMED_CHANGE : 0U;
            send(packet);
        }

        /// @brief Serializes a wire struct and hands the bytes to the link.
        /// @tparam Packet wire struct type
        /// @param packet packet to send
        template <typename Packet> void send(const Packet &packet)
        {
            std::array<std::uint8_t, sizeof(Packet)> bytes{};
            std::memcpy(bytes.data(), &packet, sizeof(Packet));
            m_sender.send(bytes.data(), bytes.size());
            ++m_answerCount;
        }

        FlightCore &m_core;                ///< registry owner, not owned
        AbsTelemetrySender &m_sender;      ///< answer link, not owned
        std::size_t m_listCursor = 0U;     ///< next table index to describe
        bool m_listPending = false;        ///< a list request is still unrolling
        std::uint32_t m_requestCount = 0U; ///< tuning requests understood
        std::uint32_t m_answerCount = 0U;  ///< acks and infos emitted
    };
} // namespace mark4
