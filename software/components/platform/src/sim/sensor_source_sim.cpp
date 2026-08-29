#include "platform_sim/sensor_source_sim.hpp"

#include <cstdint>
#include <cstring>

#include "protocol/envelope.hpp"

namespace mark4
{
    FrameWait SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        mark4_SimSensor sensor;
        std::uint32_t src = 0U;
        while (true)
        {
            if (!m_link.waitSensor(sensor, src))
            {
                return FrameWait::TIMEOUT; // idle link: the caller decides
            }
            if (m_link.plantAlive() && src != m_link.plant())
            {
                /* Two plants on the LAN: the first one heard drives this
                   process until the transport forgets it, the other is
                   somebody else's. */
                ++m_foreignFrames;
                continue;
            }
            if (src != m_link.plant())
            {
                /* Only a validated sensor message may steer the replies:
                   a stray payload must never redirect them. A plant taking
                   over from a forgotten one is a new plant, whatever its
                   clock says. */
                if (m_link.plant() != 0U)
                {
                    ++m_plantRestarts;
                    m_timestampSeen = false;
                }
                m_link.setPlant(src);
            }

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
                ++m_plantRestarts;
            }

            frameOut.timestampUs = sensor.timestamp_us;
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
} // namespace mark4
