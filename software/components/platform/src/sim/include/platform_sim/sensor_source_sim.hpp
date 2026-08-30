#pragma once

/// @file
/// @brief Sensor source of the sim variant: the plant's SimSensor envelopes
///        when a plant drives, clock-paced frames without sensors otherwise.

#include <cstdint>

#include "platform/clock.hpp"
#include "platform/sensor_source.hpp"
#include "platform_sim/plant_link.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// The single owner of "is there a plant". waitFrame() always returns
    /// a frame, at the nominal loop rate:
    /// - with a plant, its SimSensor cadence drives the loop, lockstep
    ///   included, and the frame carries the simulated time and fresh
    ///   sensors (imuValid and baroValid true);
    /// - without one, the frame is paced by the platform clock, carries
    ///   that clock's time, zeros, and both flags false: the flight core
    ///   integrates nothing and never arms, exactly like a board booting
    ///   without its sensors, while the command path keeps being served.
    ///
    /// The plant is whichever node sends the first SimSensor that
    /// validates, until it stays silent for PLANT_SILENCE_US: it is then
    /// dropped and the clock takes over, until a plant speaks again. Every
    /// such change of driver, and a plant whose simulated clock went
    /// backwards, changes the time base of the frames: sessionCount() rises
    /// and the composition root restarts the flight core on it. Appear,
    /// loss and restart are sim/plant INFO lines.
    class SensorSourceSim final : public AbsSensorSource
    {
      public:
        /// Loop rate without a plant, the flight rate of the real board.
        static constexpr std::uint32_t FRAME_RATE_HZ = 500U;

        /// Frame period without a plant [us].
        static constexpr std::uint64_t TICK_US = 1000000U / FRAME_RATE_HZ;

        /// A plant silent this long is gone [us]: paused, killed or
        /// restarting. Short enough that a Godot restart is a clean loss
        /// followed by a clean adoption, long enough that lockstep jitter
        /// never counts.
        static constexpr std::uint64_t PLANT_SILENCE_US = 500000U;

        /// @param link plant link the sensor envelopes arrive on
        /// @param clock platform clock that paces and stamps the frames
        ///        when no plant does
        SensorSourceSim(PlantLink &link, AbsClock &clock)
            : m_link(link),
              m_clock(clock)
        {
        }

        /// @brief Blocks until the plant's next sensor envelope, or until the
        ///        next clock tick when no plant drives. The RC fields of the
        ///        frame are not written here: RC arrives out-of-band through
        ///        the command receiver.
        ///
        ///        A message repeating the timestamp of the previous one is a
        ///        resend rather than a new sample: the plant asks again
        ///        because the reply to that exact tick never reached it. The
        ///        cached reply goes out again and the wait resumes, so the
        ///        flight core and the telemetry never see the same instant
        ///        twice. A SimSensor from another node while a plant drives
        ///        is counted and ignored.
        /// @param[out] frameOut the frame
        /// @return always FRAME
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return true while a plant drives the frames
        [[nodiscard]] bool plantConnected() const
        {
            return m_link.plant() != 0U;
        }

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

        /// @return changes of the time base of the frames so far: a plant
        ///         adopted, a plant lost (back to the platform clock), a
        ///         plant whose simulated clock went backwards. A rise means
        ///         nothing the flight core remembers about time applies
        ///         anymore: the composition root rebuilds it, exactly like
        ///         on a world reset.
        [[nodiscard]] std::uint32_t sessionCount() const
        {
            return m_sessions;
        }

        /// @return frames paced by the platform clock, without sensors
        [[nodiscard]] std::uint32_t clockFrameCount() const
        {
            return m_clockFrames;
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
        /// @brief Fills a frame without sensors, stamped by the clock.
        void fillClockFrame(mark4::SensorFrame &frameOut, std::uint64_t nowUs);

        PlantLink &m_link; ///< plant link, owned by the composition root
        AbsClock &m_clock; ///< platform clock, owned by the composition root
        mark4_PlantTruth m_truth = mark4_PlantTruth_init_zero; ///< truth of the last frame
        std::uint32_t m_resetCount = 0U;       ///< reset counter of the last message
        std::uint32_t m_lockstepTimeouts = 0U; ///< plant timeout count of the last message
        std::uint64_t m_lastTimestampUs = 0U;  ///< timestamp of the last accepted message
        bool m_timestampSeen = false;          ///< true once a message was accepted
        std::uint64_t m_lastPlantUs = 0U;      ///< clock time of the last plant message
        std::uint64_t m_nextTickUs = 0U;       ///< clock time of the next clock frame
        bool m_tickStarted = false;            ///< m_nextTickUs is meaningful
        std::uint32_t m_duplicateFrames = 0U;  ///< resends answered again, never stepped
        std::uint32_t m_foreignFrames = 0U;    ///< sensor messages from another node
        std::uint32_t m_sessions = 0U;         ///< time base changes
        std::uint32_t m_clockFrames = 0U;      ///< frames paced by the clock
    };
} // namespace mark4
