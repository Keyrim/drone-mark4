#pragma once

/// @file
/// @brief Sensor source of the sim variant: SimSensor envelopes off the
///        plant link.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_sim/plant_link.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Turns the SimSensor envelopes the plant link delivers into sensor
    /// frames. The plant drives the cadence: waitFrame() blocks on the link,
    /// and the timestamp of the frame is the simulation time carried by the
    /// message.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        /// @param link plant link the sensor envelopes arrive on
        explicit SensorSourceSim(PlantLink &link)
            : m_link(link)
        {
        }

        /// @brief Blocks until a valid sensor envelope arrives from the plant.
        ///        The first node whose SimSensor validates is THE plant until
        ///        the transport forgets it; a SimSensor from any other node is
        ///        counted and ignored. The RC fields of the frame are not
        ///        written here: RC arrives out-of-band through the command
        ///        receiver.
        ///
        ///        A message repeating the timestamp of the previous one is a
        ///        resend rather than a new sample: the plant asks again
        ///        because the reply to that exact tick never reached it. The
        ///        cached reply goes out again and the wait resumes, so the
        ///        flight core and the telemetry never see the same instant
        ///        twice. A timestamp going backwards, or a new plant node, is
        ///        a plant that restarted: counted, and never a resend.
        /// @param[out] frameOut frame decoded from the message
        /// @return FRAME when a message was decoded, TIMEOUT when the link
        ///         stayed idle for its wait timeout; never EXHAUSTED (a plant
        ///         may always reconnect)
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return reset counter carried by the last decoded message. The
        ///         plant increments it on every world reset (teleport), a
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

        /// @return sensor messages ignored because another node than the
        ///         plant sent them
        [[nodiscard]] std::uint32_t foreignFrameCount() const
        {
            return m_foreignFrames;
        }

        /// @return plants seen restarting (a timestamp going backwards, or
        ///         another node taking over). A rise means a different plant,
        ///         whose simulated clock and world both started over: the
        ///         composition root rebuilds the flight core on it, exactly
        ///         like on a reset.
        [[nodiscard]] std::uint32_t plantRestarts() const
        {
            return m_plantRestarts;
        }

        /// @return lockstep timeouts the plant has counted since it
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
        PlantLink &m_link; ///< plant link, owned by the composition root
        mark4_PlantTruth m_truth = mark4_PlantTruth_init_zero; ///< truth of the last frame
        std::uint32_t m_resetCount = 0U;       ///< reset counter of the last message
        std::uint32_t m_lockstepTimeouts = 0U; ///< plant timeout count of the last message
        std::uint64_t m_lastTimestampUs = 0U;  ///< timestamp of the last accepted message
        bool m_timestampSeen = false;          ///< true once a message was accepted
        std::uint32_t m_duplicateFrames = 0U;  ///< resends answered again, never stepped
        std::uint32_t m_foreignFrames = 0U;    ///< sensor messages from another node
        std::uint32_t m_plantRestarts = 0U;    ///< plants that started over
    };
} // namespace mark4
