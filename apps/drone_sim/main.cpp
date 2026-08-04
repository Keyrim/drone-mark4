/// @file
/// @brief drone_sim entry point: parses arguments, builds the app, runs it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "drone_sim_app.hpp"

namespace
{
    constexpr std::uint32_t DEFAULT_ITERATIONS = 500U;
    constexpr int STRTOL_BASE = 10;
} // namespace

int main(int argc, char **argv)
{
    std::uint32_t iterations = DEFAULT_ITERATIONS;
    if (argc > 1)
    {
        char *end = nullptr;
        const long parsed = std::strtol(argv[1], &end, STRTOL_BASE);
        if (end == argv[1] || *end != '\0' || parsed <= 0L)
        {
            static_cast<void>(std::fprintf(stderr, "usage: %s [iterations > 0]\n", argv[0]));
            return 1;
        }
        iterations = static_cast<std::uint32_t>(parsed);
    }

    mark4::DroneSimApp app(iterations);
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_sim: initialization failed\n"));
        return 1;
    }

    const std::uint32_t steps = app.run();

    const auto &telemetry = app.accessTelemetrySender();
    const auto &motor = app.accessMotorSink().last().motor;
    std::printf("drone_sim: %u steps @ %llu us/frame, %u telemetry packets (%zu bytes)\n",
                steps,
                static_cast<unsigned long long>(mark4::DroneSimApp::FRAME_PERIOD_US),
                telemetry.packetCount(),
                telemetry.byteCount());
    std::printf("drone_sim: last motors [%.2f %.2f %.2f %.2f], clock %llu us\n",
                static_cast<double>(motor[0]),
                static_cast<double>(motor[1]),
                static_cast<double>(motor[2]),
                static_cast<double>(motor[3]),
                static_cast<unsigned long long>(app.accessClock().nowUs()));
    return 0;
}
