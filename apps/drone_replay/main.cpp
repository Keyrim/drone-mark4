/// @file
/// @brief drone_replay entry point: parses arguments, builds the app, runs it.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone_replay_app.hpp"
#include "platform_replay/sensor_source_replay.hpp"
#include "protocol/telemetry.hpp"

namespace
{
    constexpr float DEFAULT_SPEED = 1.0f;

    void printUsage(const char *program)
    {
        static_cast<void>(
            std::fprintf(stderr, "usage: %s <log.m4bb> [--speed <factor>|max]\n", program));
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4)
    {
        printUsage(argv[0]);
        return 1;
    }

    float speed = DEFAULT_SPEED;
    if (argc == 4)
    {
        if (std::strcmp(argv[2], "--speed") != 0)
        {
            printUsage(argv[0]);
            return 1;
        }
        if (std::strcmp(argv[3], "max") == 0)
        {
            speed = mark4::SensorSourceReplay::SPEED_MAX;
        }
        else
        {
            char *end = nullptr;
            speed = std::strtof(argv[3], &end);
            if (end == argv[3] || *end != '\0' || speed <= 0.0f)
            {
                printUsage(argv[0]);
                return 1;
            }
        }
    }

    mark4::DroneReplayApp app(argv[1], speed);
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_replay: initialization failed\n"));
        return 1;
    }

    std::printf("drone_replay: replaying %s, telemetry broadcast on udp/%u\n",
                argv[1],
                static_cast<unsigned>(mark4::TELEMETRY_PORT));

    const std::uint32_t steps = app.run();
    const auto &telemetry = app.accessTelemetrySender();
    std::printf("drone_replay: %u frames replayed, %u telemetry packets (%zu bytes)\n",
                steps,
                telemetry.packetCount(),
                telemetry.byteCount());
    return 0;
}
