#include "drone_sim_app.hpp"

#include <array>
#include <cstring>

#include "protocol/telemetry.hpp"
#include "protocol/version.hpp"

namespace
{
    constexpr std::uint32_t TELEMETRY_DECIMATION = 50U;
} // namespace

namespace mark4
{
    DroneSimApp::DroneSimApp(std::uint32_t iterations)
        : m_sensorSource(m_clock, FRAME_PERIOD_US, iterations)
    {
    }

    /* No fallible service yet: kept as an instance method because it will touch
       members as soon as one shows up (sockets, files). */
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    bool DroneSimApp::init()
    {
        return true;
    }

    std::uint32_t DroneSimApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        while (m_sensorSource.waitFrame(frame))
        {
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            if (m_core.stepCount() % TELEMETRY_DECIMATION == 0U)
            {
                sendTelemetry(frame, actuators);
            }
        }
        return m_core.stepCount();
    }

    void DroneSimApp::sendTelemetry(const mark4::SensorFrame &frame,
                                    const mark4::ActuatorFrame &actuators)
    {
        mark4::TelemetryPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.timestampUs = frame.timestampUs;
        packet.gyroRadS = frame.gyroRadS;
        packet.motor = actuators.motor;

        std::array<std::uint8_t, sizeof(packet)> wire{};
        std::memcpy(wire.data(), &packet, sizeof(packet));
        m_telemetrySender.send(wire.data(), wire.size());
    }
} // namespace mark4
