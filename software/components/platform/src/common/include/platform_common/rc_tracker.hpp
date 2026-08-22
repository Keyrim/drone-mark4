#pragma once

/// @file
/// @brief RC decode and fail-safe, shared by every composition: the code
///        path guarding a real flight is therefore the one exercised in
///        every simulated flight too. RC never travels inside a sensor
///        frame; it arrives out-of-band on a command receiver, and the
///        composition root grafts the tracked state onto the frame it is
///        about to step the core with.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/types.hpp"
#include "platform/command_receiver.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    // This adapter is where the wire mode byte and the flight-core enum meet:
    // flight-core never includes a protocol header, so the two definitions are
    // pinned to each other here, once, instead of drifting apart silently.
    static_assert(static_cast<std::uint8_t>(PilotMode::MANUAL) == RC_MODE_MANUAL,
                  "the manual mode must keep its wire value");
    static_assert(static_cast<std::uint8_t>(PilotMode::ALTITUDE_AUTO) == RC_MODE_ALTITUDE_AUTO,
                  "the altitude-auto mode must keep its wire value");

    /// Tracks the last pilot state seen on a command receiver and decides
    /// when silence means kill. Owns no clock: the caller passes the
    /// timestamp of the frame being processed, so the fail-safe is paced by
    /// data arrival exactly like the flight core is.
    class RcTracker
    {
      public:
        /// Fail-safe: silence on the RC uplink longer than this reverts to
        /// the safe state (kill engaged, disarmed, throttle zero). Five
        /// missed packets at the 10 Hz the sender streams at.
        static constexpr std::uint64_t RC_TIMEOUT_US = 500000U;

        /// @param receiver command uplink, owned by the composition root
        explicit RcTracker(AbsCommandReceiver &receiver)
            : m_receiver(receiver)
        {
        }

        /// @brief Drains the receiver, consuming every RC packet, and hands
        ///        the first packet that is not one back to the caller. Call
        ///        it in a loop until it returns 0: the packets it does not
        ///        understand belong to the composition (a reboot command,
        ///        tuning later), not here.
        /// @param[out] otherOut receives the first non-RC packet, valid only
        ///        when returning > 0
        /// @param capacity size of otherOut in bytes
        /// @param nowUs timestamp of the frame being processed [us]
        /// @return size of the non-RC packet handed out, 0 once drained
        std::size_t pump(std::uint8_t *otherOut, std::size_t capacity, std::uint64_t nowUs)
        {
            for (;;)
            {
                const std::size_t size = m_receiver.poll(otherOut, capacity);
                if (size == 0U)
                {
                    return 0U; // drained
                }
                if (size != RC_COMMAND_PACKET_SIZE ||
                    !hasHeader(otherOut, size, PacketType::RC_COMMAND))
                {
                    return size; // not ours: the caller decides what it is
                }

                RcCommandPacket command{};
                std::memcpy(&command, otherOut, sizeof(command));
                m_rc.killSwitch = command.killSwitch != 0U;
                m_rc.armSwitch = command.armSwitch != 0U;
                m_rc.throttle = command.throttle;
                /* An unknown mode byte decodes to the safest mode rather than
                   being rejected: a newer ground station selecting a mode this
                   build does not know must degrade to direct thrust, never to
                   a mode that flies on its own. */
                m_rc.mode = command.mode == RC_MODE_ALTITUDE_AUTO ? PilotMode::ALTITUDE_AUTO
                                                                  : PilotMode::MANUAL;
                m_lastRcUs = nowUs;
                m_everReceived = true;
                ++m_rcPacketCount;
            }
        }

        /// @brief Writes the tracked pilot state into a frame, or the safe
        ///        state when the fail-safe is active.
        /// @param[in,out] frameInOut frame about to be stepped; its
        ///        timestamp decides whether the uplink went silent
        void graft(SensorFrame &frameInOut) const
        {
            frameInOut.rc = failsafeActive(frameInOut.timestampUs) ? RcInput{} : m_rc;
        }

        /// @param nowUs timestamp of the frame being processed [us]
        /// @return true while no packet was ever received, or when the last
        ///         one is older than RC_TIMEOUT_US
        [[nodiscard]] bool failsafeActive(std::uint64_t nowUs) const
        {
            /* Unsigned subtraction: a clock that went backwards wraps to a
               huge age, which lands on the safe side of the comparison. */
            return !m_everReceived || (nowUs - m_lastRcUs) > RC_TIMEOUT_US;
        }

        /// @return RC packets consumed since construction
        [[nodiscard]] std::uint32_t rcPacketCount() const
        {
            return m_rcPacketCount;
        }

      private:
        AbsCommandReceiver &m_receiver;     ///< uplink, not owned
        RcInput m_rc;                       ///< last state seen, safe defaults
        std::uint64_t m_lastRcUs = 0U;      ///< frame time of that state [us]
        bool m_everReceived = false;        ///< true once one packet arrived
        std::uint32_t m_rcPacketCount = 0U; ///< RC packets consumed
    };
} // namespace mark4
