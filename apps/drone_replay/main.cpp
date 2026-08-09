/// @file
/// @brief drone_replay entry point: parses arguments, builds the app, runs it.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone_replay_app.hpp"
#include "flight_core/throw_detector.hpp"
#include "platform_common/session_id.hpp"
#include "platform_replay/sensor_source_replay.hpp"
#include "protocol/ports.hpp"

namespace
{
    constexpr float DEFAULT_SPEED = 1.0f;
    constexpr float RAD_TO_DEG = 57.29578f;
    constexpr double US_PER_S = 1e6;
    constexpr int STRTOL_BASE = 10;
    constexpr long MAX_PORT = 65535L;

    void printUsage(const char *program)
    {
        static_cast<void>(
            std::fprintf(stderr,
                         "usage: %s <log.m4bb> [--speed <factor>|max] [--announce-port N]\n"
                         "  --speed          replay tempo, 'max' to run unpaced (default 1)\n"
                         "  --announce-port  UDP port the process announce goes to (default %u)\n",
                         program,
                         static_cast<unsigned>(mark4::ANNOUNCE_PORT)));
    }

    /// @brief Parses the replay tempo.
    /// @param text argument text
    /// @param valueOut receives the factor
    /// @return true when the text is 'max' or a strictly positive number
    bool parseSpeed(const char *text, float &valueOut)
    {
        if (std::strcmp(text, "max") == 0)
        {
            valueOut = mark4::SensorSourceReplay::SPEED_MAX;
            return true;
        }
        char *end = nullptr;
        valueOut = std::strtof(text, &end);
        return end != text && *end == '\0' && valueOut > 0.0f;
    }

    /// @brief Parses a port number.
    /// @param text argument text
    /// @param valueOut receives the port
    /// @return true when the text is a port in [1, 65535]
    bool parsePort(const char *text, std::uint16_t &valueOut)
    {
        char *end = nullptr;
        const long parsed = std::strtol(text, &end, STRTOL_BASE);
        if (end == text || *end != '\0' || parsed <= 0L || parsed > MAX_PORT)
        {
            return false;
        }
        valueOut = static_cast<std::uint16_t>(parsed);
        return true;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argv[1][0] == '-')
    {
        printUsage(argv[0]);
        return 1;
    }

    float speed = DEFAULT_SPEED;
    std::uint16_t announcePort = mark4::ANNOUNCE_PORT;
    for (int index = 2; index < argc; ++index)
    {
        const bool hasValue = index + 1 < argc;
        if (std::strcmp(argv[index], "--speed") == 0 && hasValue)
        {
            if (!parseSpeed(argv[++index], speed))
            {
                printUsage(argv[0]);
                return 1;
            }
        }
        else if (std::strcmp(argv[index], "--announce-port") == 0 && hasValue)
        {
            if (!parsePort(argv[++index], announcePort))
            {
                printUsage(argv[0]);
                return 1;
            }
        }
        else
        {
            printUsage(argv[0]);
            return 1;
        }
    }

    const std::uint32_t sessionId = mark4::makeSessionId();
    mark4::DroneReplayApp app(argv[1], speed, sessionId, announcePort);
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_replay: initialization failed\n"));
        return 1;
    }

    std::printf("drone_replay: replaying %s, telemetry broadcast on udp/%u, "
                "announcing on udp/%u as session %u\n",
                argv[1],
                static_cast<unsigned>(mark4::TELEMETRY_PORT),
                static_cast<unsigned>(announcePort),
                static_cast<unsigned>(sessionId));

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
