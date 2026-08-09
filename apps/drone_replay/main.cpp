/// @file
/// @brief drone_replay entry point: parses arguments, builds the app, runs it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone_replay_app.hpp"
#include "flight_core/throw_detector.hpp"
#include "platform_replay/sensor_source_replay.hpp"
#include "protocol/ports.hpp"

namespace
{
    constexpr float DEFAULT_SPEED = 1.0f;
    constexpr float RAD_TO_DEG = 57.29578f;
    constexpr double US_PER_S = 1e6;

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

    const mark4::ThrowDetector &detector = app.accessFlightCore().throwDetector();
    if (detector.throwCount() > 0U)
    {
        std::printf("drone_replay: %u throw(s) detected, last release %.2f m/s at t=%.3f s, "
                    "predicted apex %.2f m at t=%.3f s\n",
                    detector.throwCount(),
                    static_cast<double>(detector.releaseVelocityMps()),
                    static_cast<double>(detector.releaseTimestampUs()) / US_PER_S,
                    static_cast<double>(detector.apexAltitudeM()),
                    static_cast<double>(detector.apexTimestampUs()) / US_PER_S);
    }
    else
    {
        std::printf("drone_replay: no throw detected\n");
    }

    const mark4::Quaternion &q = app.accessFlightCore().attitude();
    const float roll =
        std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float pitch = std::asin(2.0f * (q.w * q.y - q.z * q.x));
    const float yaw =
        std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    const auto bias = app.accessFlightCore().gyroBiasRadS();
    std::printf("drone_replay: final altitude %.2f m, vertical velocity %.2f m/s\n",
                static_cast<double>(app.accessFlightCore().altitudeM()),
                static_cast<double>(app.accessFlightCore().verticalVelocityMps()));
    std::printf("drone_replay: final attitude roll %.1f pitch %.1f yaw %.1f [deg], "
                "gyro bias [%.4f %.4f %.4f] rad/s\n",
                static_cast<double>(roll * RAD_TO_DEG),
                static_cast<double>(pitch * RAD_TO_DEG),
                static_cast<double>(yaw * RAD_TO_DEG),
                static_cast<double>(bias[0]),
                static_cast<double>(bias[1]),
                static_cast<double>(bias[2]));
    return 0;
}
