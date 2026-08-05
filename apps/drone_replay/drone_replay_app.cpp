#include "drone_replay_app.hpp"

#include "flight_core/telemetry.hpp"

namespace
{
    constexpr std::uint32_t TELEMETRY_DECIMATION = 10U;
} // namespace

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
        return m_telemetrySender.open();
    }

    std::uint32_t DroneReplayApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        while (m_sensorSource.waitFrame(frame))
        {
            m_core.step(frame, actuators);
            if (m_core.stepCount() % TELEMETRY_DECIMATION == 0U)
            {
                sendTelemetry(frame, actuators);
            }
        }
        return m_core.stepCount();
    }

    void DroneReplayApp::sendTelemetry(const mark4::SensorFrame &frame,
                                       const mark4::ActuatorFrame &actuators)
    {
        const auto wire = mark4::packTelemetry(frame, actuators, m_core);
        m_telemetrySender.send(wire.data(), wire.size());
    }
} // namespace mark4
