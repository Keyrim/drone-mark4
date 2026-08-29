#include "platform_sim/sensor_source_sim.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol/envelope.hpp"

namespace mark4
{
    namespace
    {
        /// Larger than any Envelope, so an oversized datagram comes out with
        /// its real size instead of being truncated into a valid one.
        constexpr std::size_t RECEIVE_BUFFER_SIZE = MAX_ENVELOPE_SIZE + 64U;
    } // namespace

    FrameWait SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        std::array<std::uint8_t, RECEIVE_BUFFER_SIZE> wire{};

        while (true)
        {
            const std::size_t received = m_link.receive(wire.data(), wire.size());
            if (received == 0U)
            {
                return FrameWait::TIMEOUT; // idle link: the caller decides
            }
            mark4_Envelope envelope;
            if (!decodeEnvelope(wire.data(), received, envelope) ||
                envelope.which_body != mark4_Envelope_sim_sensor_tag)
            {
                continue; // not a sensor message we understand: keep waiting
            }
            const mark4_SimSensor &sensor = envelope.body.sim_sensor;

            if (m_timestampSeen && sensor.timestamp_us == m_lastTimestampUs)
            {
                /* The simulator is asking again for the answer to a tick it
                   already sent: the reply was lost, not the sample. Answering
                   twice is correct, stepping twice would fabricate a frame
                   the plant never produced. */
                m_link.acceptLastSender();
                static_cast<void>(m_link.repeatLastReply());
                ++m_duplicateFrames;
                continue;
            }
            if (m_timestampSeen && sensor.timestamp_us < m_lastTimestampUs)
            {
                /* A different plant: its simulated clock starts over, so
                   nothing this source remembers about the previous one
                   applies any more. */
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
            // Only a validated sensor message may steer the motor replies:
            // a stray datagram on the port must not stall the lockstep.
            m_link.acceptLastSender();
            return FrameWait::FRAME;
        }
    }
} // namespace mark4
