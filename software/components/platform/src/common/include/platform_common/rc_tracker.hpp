#pragma once

/// @file
/// @brief RC decode and fail-safe, shared by every composition: the code
///        path guarding a real flight is therefore the one exercised in
///        every simulated flight too. RC never travels inside a sensor
///        frame; it arrives out-of-band as an Rc message on the command
///        receiver, and the composition root grafts the tracked state onto
///        the frame it is about to step the core with.

#include <cstdint>

#include "flight_core/types.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Tracks the last pilot state seen and decides when silence means kill.
    /// Owns no clock: the caller passes the timestamp of the frame being
    /// processed, so the fail-safe is paced by data arrival exactly like the
    /// flight core is.
    class RcTracker
    {
      public:
        /// Fail-safe: silence on the RC uplink longer than this reverts to
        /// the safe state (kill engaged, disarmed, throttle zero). Five
        /// missed messages at the 10 Hz the sender streams at.
        static constexpr std::uint64_t RC_TIMEOUT_US = 500000U;

        /// @brief Takes one Rc message the composition drained.
        /// @param rc decoded message
        /// @param nowUs timestamp of the frame being processed [us]
        void onRc(const mark4_Rc &rc, std::uint64_t nowUs)
        {
            m_rc.killSwitch = rc.kill;
            m_rc.armSwitch = rc.arm;
            m_rc.throttle = rc.throttle;
            /* An unknown mode decodes to the safest mode rather than being
               rejected: a newer ground station selecting a mode this build
               does not know must degrade to direct thrust, never to a mode
               that flies on its own. */
            m_rc.mode = rc.mode == mark4_RcMode_RC_ALTITUDE_AUTO ? PilotMode::ALTITUDE_AUTO
                                                                 : PilotMode::MANUAL;
            m_lastRcUs = nowUs;
            m_everReceived = true;
            ++m_rcPacketCount;
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
        /// @return true while no message was ever received, or when the last
        ///         one is older than RC_TIMEOUT_US
        [[nodiscard]] bool failsafeActive(std::uint64_t nowUs) const
        {
            /* Unsigned subtraction: a clock that went backwards wraps to a
               huge age, which lands on the safe side of the comparison. */
            return !m_everReceived || (nowUs - m_lastRcUs) > RC_TIMEOUT_US;
        }

        /// @return RC messages consumed since construction
        [[nodiscard]] std::uint32_t rcPacketCount() const
        {
            return m_rcPacketCount;
        }

      private:
        RcInput m_rc;                       ///< last state seen, safe defaults
        std::uint64_t m_lastRcUs = 0U;      ///< frame time of that state [us]
        bool m_everReceived = false;        ///< true once one message arrived
        std::uint32_t m_rcPacketCount = 0U; ///< RC messages consumed
    };
} // namespace mark4
