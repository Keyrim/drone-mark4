#include "drone_replay_app.hpp"

#include <array>
#include <cstddef>
#include <cstring>

#include "protocol/telemetry.hpp"
#include "protocol/version.hpp"

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

    // TODO(tmagne): move the internal-to-wire conversion into flight-core so
    // the apps stop duplicating it (drone_sim carries the same code).
    void DroneReplayApp::sendTelemetry(const mark4::SensorFrame &frame,
                                       const mark4::ActuatorFrame &actuators)
    {
        mark4::TelemetryPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.timestampUs = frame.timestampUs;

        std::array<std::uint8_t, sizeof(packet)> wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));
        /* The std::array members sit at odd offsets in the packed struct:
           writing them through it would bind a reference to a misaligned
           address, so they go straight into the datagram bytes. */
        std::memcpy(wire.data() + offsetof(mark4::TelemetryPacket, gyroRadS),
                    frame.gyroRadS.data(),
                    sizeof(frame.gyroRadS));
        std::memcpy(wire.data() + offsetof(mark4::TelemetryPacket, motor),
                    actuators.motor.data(),
                    sizeof(actuators.motor));
        m_telemetrySender.send(wire.data(), wire.size());
    }
} // namespace mark4
