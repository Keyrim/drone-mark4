#include "drone_sim_app.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

#include "flight_core/throw_detector.hpp"
#include "protocol/ports.hpp"

namespace
{
    constexpr double US_PER_S = 1e6;

    /// Larger than any command packet, so an oversized datagram comes out
    /// with its real size instead of being truncated into a valid one.
    constexpr std::size_t RC_BUFFER_SIZE = 64U;

    /// Permissions of the log directory when it has to be created: rwxr-xr-x.
    constexpr mode_t LOG_DIRECTORY_MODE = 0755;

    /// @return human readable name of a flight phase, for the console
    const char *phaseName(mark4::FlightPhase phase)
    {
        switch (phase)
        {
            case mark4::FlightPhase::IDLE:
                return "idle";
            case mark4::FlightPhase::MANUAL:
                return "manual";
            case mark4::FlightPhase::ARMED:
                return "armed";
            case mark4::FlightPhase::BALLISTIC:
                return "ballistic";
            case mark4::FlightPhase::RECOVERY:
                return "recovery";
            case mark4::FlightPhase::HOVER:
                return "hover";
            case mark4::FlightPhase::CUTOFF:
                return "cutoff";
        }
        return "?";
    }

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
    DroneSimApp::DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t simPort,
                             std::uint16_t telemetryPort,
                             std::uint16_t rcPort)
        : m_maxFrames(maxFrames),
          m_simPort(simPort),
          m_telemetryPort(telemetryPort),
          m_rcPort(rcPort),
          m_sensorSource(m_simLink),
          m_motorSink(m_simLink),
          m_logFilePath(makeLogFilePath()),
          m_logSink(m_logFilePath.data()),
          m_blackbox(m_logSink)
    {
    }

    bool DroneSimApp::init()
    {
        if (!m_simLink.open(m_simPort, IDLE_TIMEOUT_MS))
        {
            return false;
        }
        if (!m_telemetrySender.open(m_telemetryPort))
        {
            return false;
        }
        // The receive timeout is irrelevant here: only receiveNonBlocking()
        // is ever called on this link, so it never waits.
        if (!m_rcLink.open(m_rcPort, 0U))
        {
            return false;
        }
        // No mirror: the announce port has no consumer that cannot share a
        // bound port, and its +2 neighbor must not see stray traffic.
        if (!m_announceSender.open(ANNOUNCE_PORT, /*mirror=*/false))
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

        std::uint32_t steps = 0U;
        std::uint32_t announcedThrows = 0U;
        mark4::FlightPhase previousPhase = mark4::FlightPhase::IDLE;
        std::uint8_t lastResetCount = 0U;
        bool resetCountSeen = false;
        while (steps < m_maxFrames)
        {
            if (m_sensorSource.waitFrame(frame) != mark4::FrameWait::FRAME)
            {
                // TIMEOUT after IDLE_TIMEOUT_MS of silence: the simulator is
                // gone, or never came. This composition treats it as the end
                // of the run; main() turns a zero-frame run into a failure.
                break;
            }
            // A simulator world reset teleports the drone: no estimator can
            // (or should) track that, so the flight core restarts from
            // scratch, exactly like a power cycle on the bench.
            if (resetCountSeen && m_sensorSource.resetCount() != lastResetCount)
            {
                m_core = mark4::FlightCore{};
                announcedThrows = 0U;
                previousPhase = mark4::FlightPhase::IDLE;
                std::printf("drone_sim: simulator reset at t=%.3f s: flight core restarted\n",
                            static_cast<double>(frame.timestampUs) / US_PER_S);
                static_cast<void>(std::fflush(stdout));
            }
            lastResetCount = m_sensorSource.resetCount();
            resetCountSeen = true;

            // Drain the command uplink. Nothing in this composition consumes
            // the packets the tracker hands back yet (reboot, tuning later),
            // so they are dropped on the floor.
            std::array<std::uint8_t, RC_BUFFER_SIZE> command{};
            while (m_rcTracker.pump(command.data(), command.size(), frame.timestampUs) != 0U)
            {
            }
            // Grafted on every frame, not only when a packet arrived: the
            // frame is reused across iterations, so skipping this would leave
            // the previous iteration's RC on it and hide the fail-safe.
            m_rcTracker.graft(frame);

            ++steps;
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            m_blackbox.record(frame, actuators);
            m_telemetryPublisher.publish(frame, actuators, m_core);
            // Wall clock, not simulated time: one announce per second of real
            // time whatever the sim time scale, which is the cadence the
            // ground side counts on.
            m_announcePublisher.publish(m_clock.nowUs());

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

            if (m_core.flightPhase() != previousPhase)
            {
                std::printf("drone_sim: phase %s -> %s at t=%.3f s\n",
                            phaseName(previousPhase),
                            phaseName(m_core.flightPhase()),
                            static_cast<double>(frame.timestampUs) / US_PER_S);
                if (m_core.flightPhase() == mark4::FlightPhase::CUTOFF)
                {
                    std::printf("drone_sim: safety cutoff: motors stopped, release the "
                                "arm switch and lower the throttle to rearm\n");
                }
                // Flushed so the line shows up immediately even when stdout is
                // piped (VS Code debug console, redirections).
                static_cast<void>(std::fflush(stdout));
                previousPhase = m_core.flightPhase();
            }
        }
        return steps;
    }

} // namespace mark4
