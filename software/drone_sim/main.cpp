/// @file
/// @brief drone_sim entry point: parses arguments, builds the app, runs it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "drone_sim_app.hpp"
#include "transport/node_id.hpp"
#include "transport/udp_link.hpp"

// Frame budget baked in at build time (see the cache variable in
// CMakeLists.txt); 0 = no limit, the run ends with the operator or the link.
// A finite budget is a bench or CI decision, never a launch decision, so it
// is not an argument.
#ifndef DRONE_SIM_FRAME_LIMIT
#define DRONE_SIM_FRAME_LIMIT 0U
#endif

namespace
{
    constexpr std::uint32_t MAX_FRAMES = DRONE_SIM_FRAME_LIMIT;
    constexpr int STRTOL_BASE = 10;
    constexpr std::uint64_t US_PER_MS = 1000U;
    constexpr long MAX_PORT = 65535L;
    constexpr long long MAX_NODE_ID = 0xFFFFFFFFLL;

    /// Name of the emulated-flash directory when the caller does not pick
    /// one. It sits next to the binary rather than under the working
    /// directory: the slots and the boot metadata are this build's flash, so
    /// they must not change with the terminal a run was started from.
    constexpr const char *OTA_DIRECTORY_NAME = "ota_flash";

    void printUsage(const char *program)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "usage: %s [--discovery-port N] [--node-id N] [--ota-dir DIR]\n"
            "  --discovery-port  shared UDP port of the transport (default %u)\n"
            "  --node-id         transport identity, 1..4294967295 (default: random)\n"
            "  --ota-dir         directory holding the emulated firmware slots and boot\n"
            "                    metadata (default '%s' next to this binary)\n",
            program,
            static_cast<unsigned>(mark4::DISCOVERY_PORT),
            OTA_DIRECTORY_NAME));
    }

    /// @brief Builds the default emulated-flash path: OTA_DIRECTORY_NAME next
    ///        to the running binary.
    /// @param program argv[0]
    /// @param[out] pathOut buffer the path is written into
    /// @param capacity bytes available in pathOut
    void makeDefaultOtaDirectory(const char *program, char *pathOut, std::size_t capacity)
    {
        const char *lastSlash = std::strrchr(program, '/');
        if (lastSlash == nullptr)
        {
            static_cast<void>(std::snprintf(pathOut, capacity, "%s", OTA_DIRECTORY_NAME));
            return;
        }
        const auto directoryLength = static_cast<int>(lastSlash - program);
        static_cast<void>(std::snprintf(
            pathOut, capacity, "%.*s/%s", directoryLength, program, OTA_DIRECTORY_NAME));
    }

    /// @brief Parses a strictly positive integer bounded by maxValue.
    /// @return true on success, with the value stored in valueOut
    bool parsePositive(const char *text, long long maxValue, long long &valueOut)
    {
        char *end = nullptr;
        const long long parsed = std::strtoll(text, &end, STRTOL_BASE);
        if (end == text || *end != '\0' || parsed <= 0LL || parsed > maxValue)
        {
            return false;
        }
        valueOut = parsed;
        return true;
    }
} // namespace

int main(int argc, char **argv)
{
    std::uint16_t discoveryPort = mark4::DISCOVERY_PORT;
    std::uint32_t nodeId = 0U;
    std::array<char, mark4::DroneSimApp::OTA_DIRECTORY_SIZE> otaDirectory{};
    makeDefaultOtaDirectory(argv[0], otaDirectory.data(), otaDirectory.size());

    for (int i = 1; i < argc; ++i)
    {
        long long value = 0LL;
        if (std::strcmp(argv[i], "--discovery-port") == 0 && i + 1 < argc)
        {
            if (!parsePositive(argv[++i], MAX_PORT, value))
            {
                printUsage(argv[0]);
                return 1;
            }
            discoveryPort = static_cast<std::uint16_t>(value);
        }
        else if (std::strcmp(argv[i], "--node-id") == 0 && i + 1 < argc)
        {
            if (!parsePositive(argv[++i], MAX_NODE_ID, value))
            {
                printUsage(argv[0]);
                return 1;
            }
            nodeId = static_cast<std::uint32_t>(value);
        }
        else if (std::strcmp(argv[i], "--ota-dir") == 0 && i + 1 < argc)
        {
            ++i;
            if (std::snprintf(otaDirectory.data(), otaDirectory.size(), "%s", argv[i]) < 0 ||
                std::strlen(argv[i]) >= otaDirectory.size())
            {
                static_cast<void>(
                    std::fprintf(stderr, "drone_sim: --ota-dir path is too long: %s\n", argv[i]));
                return 1;
            }
        }
        else
        {
            printUsage(argv[0]);
            return 1;
        }
    }

    if (nodeId == 0U)
    {
        nodeId = mark4::randomNodeId();
        if (nodeId == 0U)
        {
            static_cast<void>(std::fprintf(stderr, "drone_sim: cannot draw a node id\n"));
            return 1;
        }
    }
    mark4::DroneSimApp app(MAX_FRAMES, discoveryPort, nodeId, otaDirectory.data());
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "drone_sim: initialization failed\n"));
        return 1;
    }

    std::printf("drone_sim: transport node %u on discovery udp/%u, waiting for a plant\n",
                static_cast<unsigned>(nodeId),
                static_cast<unsigned>(discoveryPort));

    const std::uint32_t steps = app.run();
    if (steps == 0U)
    {
        static_cast<void>(std::fprintf(
            stderr, "drone_sim: no sensor frame ever arrived: is the simulator running?\n"));
        return 1;
    }
    const std::uint64_t elapsedMs = app.accessClock().nowUs() / US_PER_MS;

    const auto &telemetry = app.accessTelemetrySender();
    const auto &motor = app.accessMotorSink().last().motor;
    std::printf(
        "drone_sim: %u steps in %llu ms, %u telemetry packets (%zu bytes), %zu nodes seen\n",
        steps,
        static_cast<unsigned long long>(elapsedMs),
        telemetry.packetCount(),
        telemetry.byteCount(),
        app.accessTransport().nodeCount());
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
    return 0;
}
