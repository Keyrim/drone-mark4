/// @file
/// @brief drone_sim entry point: parses arguments, builds the app, runs it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "drone_sim_app.hpp"
#include "protocol/sim_link.hpp"
#include "protocol/telemetry.hpp"

namespace
{
    constexpr std::uint32_t DEFAULT_MAX_FRAMES = 500U;
    constexpr int STRTOL_BASE = 10;
    constexpr std::uint64_t US_PER_MS = 1000U;
} // namespace

int main(int argc, char **argv)
{
    std::uint32_t maxFrames = DEFAULT_MAX_FRAMES;
    if (argc > 1)
    {
        char *end = nullptr;
        const long parsed = std::strtol(argv[1], &end, STRTOL_BASE);
        if (end == argv[1] || *end != '\0' || parsed <= 0L)
        {
            static_cast<void>(std::fprintf(stderr, "usage: %s [frames > 0]\n", argv[0]));
            return 1;
        }
        maxFrames = static_cast<std::uint32_t>(parsed);
    }

    mark4::DroneSimApp app(maxFrames);
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_sim: initialization failed\n"));
        return 1;
    }

    std::printf("drone_sim: waiting for sensor packets on udp/%u, telemetry broadcast on udp/%u\n",
                static_cast<unsigned>(mark4::SIM_LINK_PORT),
                static_cast<unsigned>(mark4::TELEMETRY_PORT));

    const std::uint32_t steps = app.run();
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

    const auto &logSink = app.accessLogSink();
    std::printf("drone_sim: %u blackbox records (%zu bytes) in %s\n",
                app.accessBlackbox().recordCount(),
                logSink.bytesWritten(),
                logSink.path());
    return 0;
}
