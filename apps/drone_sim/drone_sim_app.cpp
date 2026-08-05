#include "drone_sim_app.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

#include "flight_core/telemetry.hpp"
#include "flight_core/throw_detector.hpp"
#include "protocol/sim_link.hpp"

namespace
{
    constexpr std::uint32_t TELEMETRY_DECIMATION = 10U;
    constexpr double US_PER_S = 1e6;

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

        std::uint32_t announcedThrows = 0U;
        mark4::ArmState previousArmState = mark4::ArmState::DISARMED;
        while (m_core.stepCount() < m_maxFrames && m_sensorSource.waitFrame(frame))
        {
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            m_blackbox.record(frame, actuators);
            if (m_core.stepCount() % TELEMETRY_DECIMATION == 0U)
            {
                sendTelemetry(frame, actuators);
            }

            const mark4::ThrowDetector &detector = m_core.throwDetector();
            if (detector.throwCount() > announcedThrows)
            {
                announcedThrows = detector.throwCount();
                std::printf("drone_sim: throw #%u detected: release %.2f m/s at t=%.3f s, "
                            "predicted apex %.2f m at t=%.3f s\n",
                            announcedThrows,
                            static_cast<double>(detector.releaseVelocityMps()),
                            static_cast<double>(detector.releaseTimestampUs()) / US_PER_S,
                            static_cast<double>(detector.apexAltitudeM()),
                            static_cast<double>(detector.apexTimestampUs()) / US_PER_S);
                // Flushed so the line shows up immediately even when stdout is
                // piped (VS Code debug console, redirections): the whole point
                // is seeing the detection live.
                static_cast<void>(std::fflush(stdout));
            }

            if (m_core.armState() != previousArmState)
            {
                if (m_core.armState() == mark4::ArmState::CUTOFF)
                {
                    std::printf("drone_sim: safety cutoff at t=%.3f s: motors stopped, "
                                "lower the throttle to rearm\n",
                                static_cast<double>(frame.timestampUs) / US_PER_S);
                    static_cast<void>(std::fflush(stdout));
                }
                previousArmState = m_core.armState();
            }
        }
        return m_core.stepCount();
    }

    void DroneSimApp::sendTelemetry(const mark4::SensorFrame &frame,
                                    const mark4::ActuatorFrame &actuators)
    {
        const auto wire = mark4::packTelemetry(frame, actuators, m_core);
        m_telemetrySender.send(wire.data(), wire.size());
    }
} // namespace mark4
