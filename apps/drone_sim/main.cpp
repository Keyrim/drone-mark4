/// @file
/// @brief drone_sim entry point: parses arguments, builds the app, runs it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone_sim_app.hpp"
#include "platform_common/session_id.hpp"
#include "protocol/ports.hpp"

namespace
{
    constexpr std::uint32_t DEFAULT_MAX_FRAMES = 500U;
    constexpr int STRTOL_BASE = 10;
    constexpr std::uint64_t US_PER_MS = 1000U;
    constexpr long MAX_PORT = 65535L;

    void printUsage(const char *program)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "usage: %s [frames > 0] [--sim-port N] [--telemetry-port N] [--rc-port N]\n"
            "  --sim-port        UDP port the sim link listens on (default %u)\n"
            "  --telemetry-port  UDP telemetry broadcast port, mirror on +2 (default %u)\n"
            "  --rc-port         UDP port the RC command receiver binds (default %u)\n",
            program,
            static_cast<unsigned>(mark4::SIM_LINK_PORT),
            static_cast<unsigned>(mark4::TELEMETRY_PORT),
            static_cast<unsigned>(mark4::RC_COMMAND_PORT)));
    }

    /// @brief Parses a strictly positive integer bounded by maxValue.
    /// @return true on success, with the value stored in valueOut
    bool parsePositive(const char *text, long maxValue, long &valueOut)
    {
        char *end = nullptr;
        const long parsed = std::strtol(text, &end, STRTOL_BASE);
        if (end == text || *end != '\0' || parsed <= 0L || parsed > maxValue)
        {
            return false;
        }
        valueOut = parsed;
        return true;
    }
} // namespace

int main(int argc, char **argv)
{
    std::uint32_t maxFrames = DEFAULT_MAX_FRAMES;
    std::uint16_t simPort = mark4::SIM_LINK_PORT;
    std::uint16_t telemetryPort = mark4::TELEMETRY_PORT;
    std::uint16_t rcPort = mark4::RC_COMMAND_PORT;

    for (int i = 1; i < argc; ++i)
    {
        long value = 0L;
        if (std::strcmp(argv[i], "--sim-port") == 0 && i + 1 < argc)
        {
            if (!parsePositive(argv[++i], MAX_PORT, value))
            {
                printUsage(argv[0]);
                return 1;
            }
            simPort = static_cast<std::uint16_t>(value);
        }
        else if (std::strcmp(argv[i], "--telemetry-port") == 0 && i + 1 < argc)
        {
            if (!parsePositive(argv[++i], MAX_PORT, value))
            {
                printUsage(argv[0]);
                return 1;
            }
            telemetryPort = static_cast<std::uint16_t>(value);
        }
        else if (std::strcmp(argv[i], "--rc-port") == 0 && i + 1 < argc)
        {
            if (!parsePositive(argv[++i], MAX_PORT, value))
            {
                printUsage(argv[0]);
                return 1;
            }
            rcPort = static_cast<std::uint16_t>(value);
        }
        else if (parsePositive(argv[i], static_cast<long>(UINT32_MAX), value))
        {
            maxFrames = static_cast<std::uint32_t>(value);
        }
        else
        {
            printUsage(argv[0]);
            return 1;
        }
    }

    const std::uint32_t sessionId = mark4::makeSessionId();
    mark4::DroneSimApp app(maxFrames, simPort, telemetryPort, rcPort, sessionId);
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_sim: initialization failed\n"));
        return 1;
    }

    std::printf("drone_sim: waiting for sensor packets on udp/%u, telemetry broadcast on udp/%u, "
                "rc uplink on udp/%u, announcing on udp/%u as session %u\n",
                static_cast<unsigned>(simPort),
                static_cast<unsigned>(telemetryPort),
                static_cast<unsigned>(rcPort),
                static_cast<unsigned>(mark4::ANNOUNCE_PORT),
                static_cast<unsigned>(sessionId));

    const std::uint32_t steps = app.run();
    if (steps == 0U)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "drone_sim: no sensor packet ever arrived on udp/%u: is the simulator running?\n",
            static_cast<unsigned>(simPort)));
        return 1;
    }
    const std::uint64_t elapsedMs = app.accessClock().nowUs() / US_PER_MS;

    const auto &telemetry = app.accessTelemetrySender();
    const auto &motor = app.accessMotorSink().last().motor;
    std::printf("drone_sim: %u steps in %llu ms, %u telemetry packets (%zu bytes)\n",
                steps,
                static_cast<unsigned long long>(elapsedMs),
                telemetry.packetCount(),
                telemetry.byteCount());
    std::printf("drone_sim: last motors [%.2f %.2f %.2f %.2f]\n",
                static_cast<double>(motor[0]),
                static_cast<double>(motor[1]),
                static_cast<double>(motor[2]),
                static_cast<double>(motor[3]));

    const auto &tracker = app.accessRunTracker();
    std::printf("drone_sim: run %u hash %016llx (%s), %u resent frames rejected%s\n",
                static_cast<unsigned>(tracker.runId()),
                static_cast<unsigned long long>(tracker.hash()),
                tracker.sealed() ? "sealed" : "partial",
                app.accessSensorSource().duplicateFrameCount(),
                tracker.degraded() ? ", LINK DEGRADED" : "");

    const auto &logSink = app.accessLogSink();
    std::printf("drone_sim: %u blackbox records (%zu bytes) in %s\n",
                app.accessBlackbox().recordCount(),
                logSink.bytesWritten(),
                logSink.path());
    return 0;
}
