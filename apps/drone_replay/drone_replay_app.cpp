#include "drone_replay_app.hpp"

#include "protocol/ports.hpp"

namespace mark4
{
    DroneReplayApp::DroneReplayApp(const char *path, float speedFactor)
        : m_logPath(path),
          m_sensorSource(speedFactor)
    {
    }

    bool DroneReplayApp::init()
    {
        if (!m_sensorSource.init(m_logPath))
        {
            return false;
        }
        return m_telemetrySender.open(mark4::TELEMETRY_PORT);
    }

    std::uint32_t DroneReplayApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        while (m_sensorSource.waitFrame(frame) == mark4::FrameWait::FRAME)
        {
            m_core.step(frame, actuators);
            m_telemetryPublisher.publish(frame, actuators, m_core);
        }
        return m_core.stepCount();
    }
} // namespace mark4
