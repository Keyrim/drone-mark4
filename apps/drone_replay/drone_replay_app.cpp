#include "drone_replay_app.hpp"

#include "protocol/ports.hpp"

namespace mark4
{
    DroneReplayApp::DroneReplayApp(const char *path,
                                   float speedFactor,
                                   std::uint32_t sessionId,
                                   std::uint16_t announcePort)
        : m_logPath(path),
          m_sessionId(sessionId),
          m_announcePort(announcePort),
          m_sensorSource(speedFactor)
    {
    }

    bool DroneReplayApp::init()
    {
        if (!m_sensorSource.init(m_logPath))
        {
            return false;
        }
        if (!m_telemetrySender.open(mark4::TELEMETRY_PORT))
        {
            return false;
        }
        // No mirror: the announce port has no consumer that cannot share a
        // bound port, and its +2 neighbor must not see stray traffic.
        return m_announceSender.open(m_announcePort, /*mirror=*/false);
    }

    std::uint32_t DroneReplayApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        while (m_sensorSource.waitFrame(frame) == mark4::FrameWait::FRAME)
        {
            m_core.step(frame, actuators);
            m_telemetryPublisher.publish(frame, actuators, m_core);
            // Wall clock, not the recorded timestamps: the announce cadence
            // is a real-time contract with the ground side, whatever tempo
            // the file is being replayed at.
            m_announcePublisher.publish(m_clock.nowUs());
        }
        return m_core.stepCount();
    }
} // namespace mark4
