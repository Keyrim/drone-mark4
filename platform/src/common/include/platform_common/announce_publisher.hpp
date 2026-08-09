#pragma once

/// @file
/// @brief Periodic process announce, the flight-process half of ground-side
///        discovery: a consumer that hears one knows what is alive and on
///        which ports, instead of relying on hand-wired defaults.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "platform/telemetry_sender.hpp"
#include "protocol/announce.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    /// Broadcasts one AnnouncePacket per second, describing the process it
    /// belongs to. The cadence is the contract with the ground side: a
    /// consumer may treat a source it has not heard from in a few periods as
    /// gone, so the period must not drift with the flight loop rate.
    class AnnouncePublisher
    {
      public:
        /// One announce per second. The first publish() call sends
        /// immediately, so a consumer started before the flight process
        /// hears about it as soon as it exists.
        static constexpr std::uint64_t PERIOD_US = 1000000U;

        /// @param sender output the announces are broadcast on
        /// @param kind StreamSource identity of the announcing process
        /// @param telemetryPort port this process broadcasts telemetry to,
        ///        0 when it has none
        /// @param commandPort port this process listens on for commands,
        ///        0 when it has none
        AnnouncePublisher(AbsTelemetrySender &sender,
                          StreamSource kind,
                          std::uint16_t telemetryPort,
                          std::uint16_t commandPort)
            : m_sender(sender),
              m_kind(kind),
              m_telemetryPort(telemetryPort),
              m_commandPort(commandPort)
        {
        }

        /// @brief Broadcasts one announce if the period elapsed.
        /// @param nowUs wall clock reading [us]; the cadence is a real-time
        ///        contract, so a simulated clock must not drive it
        void publish(std::uint64_t nowUs)
        {
            if (m_everSent && (nowUs - m_lastSentUs) < PERIOD_US)
            {
                return;
            }
            m_lastSentUs = nowUs;
            m_everSent = true;

            AnnouncePacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::ANNOUNCE);
            packet.kind = static_cast<std::uint8_t>(m_kind);
            packet.sessionId = 0U; // 0 until session identity is assigned
            packet.telemetryPort = m_telemetryPort;
            packet.commandPort = m_commandPort;

            std::array<std::uint8_t, ANNOUNCE_PACKET_SIZE> bytes{};
            std::memcpy(bytes.data(), &packet, sizeof(packet));
            m_sender.send(bytes.data(), bytes.size());
            ++m_announceCount;
        }

        /// @return announces broadcast since construction
        [[nodiscard]] std::uint32_t announceCount() const
        {
            return m_announceCount;
        }

      private:
        AbsTelemetrySender &m_sender;       ///< output link, not owned
        StreamSource m_kind;                ///< identity of this process
        std::uint16_t m_telemetryPort;      ///< announced telemetry port
        std::uint16_t m_commandPort;        ///< announced command port
        std::uint64_t m_lastSentUs = 0U;    ///< wall time of the last send [us]
        bool m_everSent = false;            ///< true once one went out
        std::uint32_t m_announceCount = 0U; ///< announces broadcast
    };
} // namespace mark4
