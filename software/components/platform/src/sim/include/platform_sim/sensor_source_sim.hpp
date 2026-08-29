#pragma once

/// @file
/// @brief UDP sensor source for the sim variant.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_sim/udp_socket.hpp"

namespace mark4
{
    /// Turns sensor packets received on the sim link into sensor frames. The
    /// simulator drives the cadence: waitFrame() blocks on the socket, and the
    /// timestamp of the frame is the simulation time carried by the packet.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        /// @param link bound sim link the sensor packets arrive on
        explicit SensorSourceSim(UdpSocket &link)
            : m_link(link)
        {
        }

        /// @brief Blocks until a valid sensor packet arrives. Datagrams of an
        ///        unexpected size or protocol version are dropped silently and
        ///        the wait resumes. The RC fields of the frame are not written
        ///        here: RC arrives out-of-band through the command receiver.
        ///
        ///        A packet repeating the timestamp of the previous one, from
        ///        the same simulator session, is a resend rather than a new
        ///        sample: the simulator asks again because the reply to that
        ///        exact tick never reached it. The cached reply goes out
        ///        again and the wait resumes, so the flight core and the
        ///        telemetry never see the same instant twice. A new session
        ///        restarts the simulated clock, so what
        ///        this source remembers about timestamps is forgotten with
        ///        the session that produced them.
        /// @param[out] frameOut frame decoded from the packet
        /// @return FRAME when a packet was decoded, TIMEOUT when the link
        ///         stayed idle for the receive timeout of the underlying
        ///         socket; never EXHAUSTED (a simulator may always reconnect)
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return reset counter carried by the last decoded packet. The
        ///         simulator increments it on every world reset (teleport), a
        ///         sim-only event with no place in the sensor frame: the
        ///         composition root watches it and rebuilds the flight core.
        [[nodiscard]] std::uint8_t resetCount() const
        {
            return m_resetCount;
        }

        /// @return sensor packets dropped as resends of the previous tick
        [[nodiscard]] std::uint32_t duplicateFrameCount() const
        {
            return m_duplicateFrames;
        }

        /// @return identity of the simulator start the last packet came from.
        ///         A change means a different plant, whose simulated clock
        ///         and world both restarted: the composition root rebuilds
        ///         the flight core on it, exactly like on a reset.
        [[nodiscard]] std::uint32_t sessionId() const
        {
            return m_sessionId;
        }

        /// @return lockstep timeouts the simulator has counted since it
        ///         started, as carried by the last packet. Cumulative and
        ///         saturating: a rise inside a run is what matters, not the
        ///         absolute value.
        [[nodiscard]] std::uint16_t lockstepTimeouts() const
        {
            return m_lockstepTimeouts;
        }

      private:
        UdpSocket &m_link;                     ///< sim link, owned by the composition root
        std::uint8_t m_resetCount = 0U;        ///< reset counter of the last packet
        std::uint32_t m_sessionId = 0U;        ///< simulator session of the last packet
        std::uint16_t m_lockstepTimeouts = 0U; ///< plant timeout count of the last packet
        std::uint64_t m_lastTimestampUs = 0U;  ///< timestamp of the last accepted packet
        bool m_timestampSeen = false;          ///< true once a packet was accepted
        std::uint32_t m_duplicateFrames = 0U;  ///< resends answered again, never stepped
    };
} // namespace mark4
