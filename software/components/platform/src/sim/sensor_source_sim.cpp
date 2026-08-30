#include "platform_sim/sensor_source_sim.hpp"

#include <cstdint>
#include <cstring>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_SIM_PLANT, "sim/plant"};

        constexpr std::uint64_t US_PER_MS = 1000U;
    } // namespace

    FrameWait SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        mark4_SimSensor sensor;
        std::uint32_t src = 0U;
        while (true)
        {
            const std::uint64_t nowUs = m_clock.nowUs();
            if (!m_tickStarted)
            {
                m_nextTickUs = nowUs;
                m_tickStarted = true;
            }
            // With a plant the wait is bounded by its silence budget, without
            // one by the next clock tick: either way the transport keeps
            // being pumped and a plant may speak at any time.
            const bool plant = plantConnected();
            const std::uint64_t deadlineUs =
                plant ? m_lastPlantUs + PLANT_SILENCE_US : m_nextTickUs;
            if (!m_link.waitSensor(sensor, src, deadlineUs))
            {
                if (plant)
                {
                    // Paused, killed, restarting: whatever it is doing, it is
                    // not driving. The clock takes over from right now.
                    MODULE.info("plant %08x lost: silent for %lu ms, sensors invalid, frames "
                                "paced by the clock",
                                m_link.plant(),
                                static_cast<unsigned long>(PLANT_SILENCE_US / US_PER_MS));
                    m_link.setPlant(0U);
                    m_timestampSeen = false;
                    ++m_sessions;
                    m_nextTickUs = m_clock.nowUs();
                    continue;
                }
                fillClockFrame(frameOut, m_clock.nowUs());
                return FrameWait::FRAME;
            }

            if (plant && src != m_link.plant())
            {
                /* Two plants on the LAN: the one heard first drives this
                   process until it falls silent, the other is somebody
                   else's. */
                ++m_foreignFrames;
                continue;
            }
            if (!plant)
            {
                /* Only a validated sensor message may steer the replies: a
                   stray payload must never redirect them. */
                m_link.setPlant(src);
                m_timestampSeen = false;
                ++m_sessions;
                MODULE.info("plant %08x connected: sensors valid, frames paced by the plant", src);
            }
            m_lastPlantUs = m_clock.nowUs();

            if (m_timestampSeen && sensor.timestamp_us == m_lastTimestampUs)
            {
                /* The plant is asking again for the answer to a tick it
                   already sent: the reply was lost, not the sample. Answering
                   twice is correct, stepping twice would fabricate a frame
                   the plant never produced. */
                static_cast<void>(m_link.repeatLastReply());
                ++m_duplicateFrames;
                continue;
            }
            if (m_timestampSeen && sensor.timestamp_us < m_lastTimestampUs)
            {
                /* The same node, its simulated clock starting over: nothing
                   this source remembers about the previous run applies. */
                ++m_sessions;
                MODULE.info("plant %08x restarted: simulated clock back at %lu ms",
                            src,
                            static_cast<unsigned long>(sensor.timestamp_us / US_PER_MS));
            }

            frameOut.timestampUs = sensor.timestamp_us;
            frameOut.imuValid = true;
            frameOut.baroValid = true;
            std::memcpy(frameOut.gyroRadS.data(), sensor.gyro_rad_s, sizeof(frameOut.gyroRadS));
            std::memcpy(frameOut.accelMps2.data(), sensor.accel_mps2, sizeof(frameOut.accelMps2));
            frameOut.baroPa = sensor.baro_pa;
            // The RC fields of the frame are deliberately left alone: RC is
            // not a sensor reading, and the composition root grafts it from
            // its RcTracker after this call returns.
            m_resetCount = sensor.reset_count;
            m_lockstepTimeouts = sensor.lockstep_timeouts;
            if (sensor.has_truth)
            {
                m_truth = sensor.truth;
            }
            else
            {
                m_truth = mark4_PlantTruth_init_zero;
            }
            m_lastTimestampUs = sensor.timestamp_us;
            m_timestampSeen = true;
            return FrameWait::FRAME;
        }
    }

    void SensorSourceSim::fillClockFrame(mark4::SensorFrame &frameOut, std::uint64_t nowUs)
    {
        // No sensors: zeros and both flags down, the clock's time. The
        // schedule advances by one period; a loop that fell more than a
        // period behind resumes from now instead of bursting to catch up.
        frameOut.timestampUs = nowUs;
        frameOut.imuValid = false;
        frameOut.baroValid = false;
        frameOut.gyroRadS.fill(0.0f);
        frameOut.accelMps2.fill(0.0f);
        frameOut.baroPa = 0.0f;
        m_truth = mark4_PlantTruth_init_zero;
        m_nextTickUs += TICK_US;
        if (m_nextTickUs + TICK_US < nowUs)
        {
            m_nextTickUs = nowUs + TICK_US;
        }
        ++m_clockFrames;
    }
} // namespace mark4
