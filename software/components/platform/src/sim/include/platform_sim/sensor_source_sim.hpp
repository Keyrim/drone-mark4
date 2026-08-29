#pragma once

/// @file
/// @brief UDP sensor source for the sim variant.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_sim/udp_socket.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Turns the SimSensor envelopes received on the sim link into sensor
    /// frames. The simulator drives the cadence: waitFrame() blocks on the
    /// socket, and the timestamp of the frame is the simulation time carried
    /// by the message.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        /// @param link bound sim link the sensor envelopes arrive on
        explicit SensorSourceSim(UdpSocket &link)
            : m_link(link)
        {
        }

        /// @brief Blocks until a valid sensor envelope arrives. Datagrams that
        ///        do not decode to a SimSensor are dropped silently and the
        ///        wait resumes. The RC fields of the frame are not written
        ///        here: RC arrives out-of-band through the command receiver.
        ///
        ///        A message repeating the timestamp of the previous one is a
        ///        resend rather than a new sample: the simulator asks again
        ///        because the reply to that exact tick never reached it. The
        ///        cached reply goes out again and the wait resumes, so the
        ///        flight core and the telemetry never see the same instant
        ///        twice. A timestamp going backwards is a plant that
        ///        restarted, whose clock starts over: counted, and never a
        ///        resend.
        /// @param[out] frameOut frame decoded from the message
        /// @return FRAME when a message was decoded, TIMEOUT when the link
        ///         stayed idle for the receive timeout of the underlying
        ///         socket; never EXHAUSTED (a simulator may always reconnect)
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return reset counter carried by the last decoded message. The
        ///         simulator increments it on every world reset (teleport), a
        ///         sim-only event with no place in the sensor frame: the
        ///         composition root watches it and rebuilds the flight core.
        [[nodiscard]] std::uint32_t resetCount() const
        {
            return m_resetCount;
        }

        /// @return sensor messages dropped as resends of the previous tick
        [[nodiscard]] std::uint32_t duplicateFrameCount() const
        {
            return m_duplicateFrames;
        }

        /// @return plants seen restarting (a timestamp going backwards). A
        ///         rise means a different plant, whose simulated clock and
        ///         world both started over: the composition root rebuilds the
        ///         flight core on it, exactly like on a reset.
        [[nodiscard]] std::uint32_t plantRestarts() const
        {
            return m_plantRestarts;
        }

        /// @return lockstep timeouts the simulator has counted since it
        ///         started, as carried by the last message. Cumulative: a rise
        ///         inside a run is what matters, not the absolute value.
        [[nodiscard]] std::uint32_t lockstepTimeouts() const
        {
            return m_lockstepTimeouts;
        }

        /// @return the plant's exact state at the last frame, as it sent it
        [[nodiscard]] const mark4_PlantTruth &truth() const
        {
            return m_truth;
        }

      private:
        UdpSocket &m_link; ///< sim link, owned by the composition root
        mark4_PlantTruth m_truth = mark4_PlantTruth_init_zero; ///< truth of the last frame
        std::uint32_t m_resetCount = 0U;       ///< reset counter of the last message
        std::uint32_t m_lockstepTimeouts = 0U; ///< plant timeout count of the last message
        std::uint64_t m_lastTimestampUs = 0U;  ///< timestamp of the last accepted message
        bool m_timestampSeen = false;          ///< true once a message was accepted
        std::uint32_t m_duplicateFrames = 0U;  ///< resends answered again, never stepped
        std::uint32_t m_plantRestarts = 0U;    ///< timestamps seen going backwards
    };
} // namespace mark4
