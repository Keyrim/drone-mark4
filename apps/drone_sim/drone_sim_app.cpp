#include "drone_sim_app.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

#include "protocol/sim_link.hpp"
#include "protocol/telemetry.hpp"
#include "protocol/version.hpp"

namespace
{
    constexpr std::uint32_t TELEMETRY_DECIMATION = 10U;

    /// Permissions of the log directory when it has to be created: rwxr-xr-x.
    constexpr mode_t LOG_DIRECTORY_MODE = 0755;

    /// @brief Creates a directory, treating an existing one as a success.
    /// @param path directory to create
    /// @return true when the directory exists afterwards
    bool ensureDirectory(const char *path)
    {
        if (::mkdir(path, LOG_DIRECTORY_MODE) == 0 || errno == EEXIST)
        {
            return true;
        }
        static_cast<void>(std::fprintf(
            stderr, "drone_sim: mkdir failed on '%s': %s\n", path, std::strerror(errno)));
        return false;
    }

    /// @brief Builds a per-run blackbox path from the wall clock, so a run
    ///        never overwrites the previous one. The format string embeds
    ///        DroneSimApp::LOG_DIRECTORY (strftime cannot interpolate it).
    /// @return "logs/drone_sim_YYYYMMDD_HHMMSS.m4bb"
    std::array<char, mark4::DroneSimApp::LOG_PATH_SIZE> makeLogFilePath()
    {
        std::array<char, mark4::DroneSimApp::LOG_PATH_SIZE> path{};
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        static_cast<void>(::localtime_r(&now, &local));
        if (std::strftime(path.data(), path.size(), "logs/drone_sim_%Y%m%d_%H%M%S.m4bb", &local) ==
            0U)
        {
            static_cast<void>(std::snprintf(path.data(), path.size(), "logs/drone_sim.m4bb"));
        }
        return path;
    }
} // namespace

namespace mark4
{
    DroneSimApp::DroneSimApp(std::uint32_t maxFrames)
        : m_maxFrames(maxFrames),
          m_sensorSource(m_simLink),
          m_motorSink(m_simLink),
          m_logFilePath(makeLogFilePath()),
          m_logSink(m_logFilePath.data()),
          m_blackbox(m_logSink)
    {
    }

    bool DroneSimApp::init()
    {
        if (!m_simLink.open(mark4::SIM_LINK_PORT, IDLE_TIMEOUT_MS))
        {
            return false;
        }
        if (!m_telemetrySender.open())
        {
            return false;
        }
        if (!ensureDirectory(LOG_DIRECTORY))
        {
            return false;
        }
        return m_logSink.init();
    }

    std::uint32_t DroneSimApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        while (m_core.stepCount() < m_maxFrames && m_sensorSource.waitFrame(frame))
        {
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            m_blackbox.record(frame, actuators);
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
