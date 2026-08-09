/// @file
/// @brief hub entry point: parses the subcommand and its options, builds the
///        app, runs it.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hub/hub_app.hpp"

namespace
{
    /// App the signal handlers stop. A file-scope pointer is the only thing a
    /// POSIX handler can reach, and requestStop() is the only thing it calls:
    /// one atomic store, nothing else.
    std::atomic<mark4::HubApp *> G_APP{nullptr};

    /// @brief SIGINT and SIGTERM handler: asks the loop to end its iteration.
    /// @param signalNumber signal received, unused
    void onStopSignal(int signalNumber)
    {
        static_cast<void>(signalNumber);
        mark4::HubApp *app = G_APP.load();
        if (app != nullptr)
        {
            app->requestStop();
        }
    }

    /// @brief Installs the stop handlers around one app.
    /// @param app app the handlers stop
    void installSignalHandlers(mark4::HubApp &app)
    {
        G_APP.store(&app);
        static_cast<void>(std::signal(SIGINT, onStopSignal));
        static_cast<void>(std::signal(SIGTERM, onStopSignal));
    }

    /// @brief Parses a port number.
    /// @param text argument text
    /// @param valueOut receives the port
    /// @return true when the text is a port in [1, 65535]
    bool parsePort(const char *text, std::uint16_t &valueOut)
    {
        static constexpr int BASE = 10;
        static constexpr long MAX_PORT = 65535L;
        char *end = nullptr;
        const long parsed = std::strtol(text, &end, BASE);
        if (end == text || *end != '\0' || parsed <= 0L || parsed > MAX_PORT)
        {
            return false;
        }
        valueOut = static_cast<std::uint16_t>(parsed);
        return true;
    }

    /// @brief Parses a strictly positive integer.
    /// @param text argument text
    /// @param valueOut receives the value
    /// @return true when the text is a positive integer
    bool parseUnsigned(const char *text, std::uint32_t &valueOut)
    {
        static constexpr int BASE = 10;
        char *end = nullptr;
        const long parsed = std::strtol(text, &end, BASE);
        if (end == text || *end != '\0' || parsed <= 0L)
        {
            return false;
        }
        valueOut = static_cast<std::uint32_t>(parsed);
        return true;
    }

    void printUsage(const char *program)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "usage: %s serve [options]\n"
            "\n"
            "  serve         decode the streams, record them, serve the websocket\n"
            "\n"
            "options:\n"
            "  --ws-port N          websocket endpoint port (default %u)\n"
            "  --announce-port N    announce listen port (default %u)\n"
            "  --telemetry-port N   telemetry port watched by default (default %u)\n"
            "  --raw-port N         sim raw port watched by default (default %u)\n"
            "  --sim-command-port N port scenario commands are sent to (default %u)\n"
            "  --serial DEV         board UART to own, none by default\n"
            "  --baud N             board UART speed (default %u)\n"
            "  --record             open a CSV recording at startup\n"
            "  --log-dir DIR        directory recordings are written to (default logs)\n",
            program,
            static_cast<unsigned>(mark4::HubApp::WS_PORT),
            static_cast<unsigned>(mark4::ANNOUNCE_PORT),
            static_cast<unsigned>(mark4::TELEMETRY_PORT),
            static_cast<unsigned>(mark4::SIM_RAW_PORT),
            static_cast<unsigned>(mark4::SIM_COMMAND_PORT),
            mark4::HubApp::DEFAULT_SERIAL_BAUD));
    }

    /// @brief Reads the options shared by every subcommand that serves.
    /// @param argc argument count
    /// @param argv argument values
    /// @param index index of the argument to read, advanced past what is consumed
    /// @param config configuration being filled
    /// @return true when the argument was one of these options and parsed
    bool readServeOption(int argc, char **argv, int &index, mark4::HubApp::Config &config)
    {
        const char *name = argv[index];
        const bool hasValue = index + 1 < argc;
        if (std::strcmp(name, "--ws-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], config.wsPort);
        }
        if (std::strcmp(name, "--announce-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], config.announcePort);
        }
        if (std::strcmp(name, "--telemetry-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], config.telemetryPort);
        }
        if (std::strcmp(name, "--raw-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], config.simRawPort);
        }
        if (std::strcmp(name, "--sim-command-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], config.simCommandPort);
        }
        if (std::strcmp(name, "--serial") == 0 && hasValue)
        {
            config.serialDevice = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--baud") == 0 && hasValue)
        {
            return parseUnsigned(argv[++index], config.serialBaud);
        }
        if (std::strcmp(name, "--log-dir") == 0 && hasValue)
        {
            config.logDirectory = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--record") == 0)
        {
            config.recordOnStart = true;
            return true;
        }
        return false;
    }

    /// @brief Runs the serve subcommand.
    /// @param argc argument count
    /// @param argv argument values, argv[1] being the subcommand
    /// @return process exit code
    int runServe(int argc, char **argv)
    {
        mark4::HubApp::Config config;
        for (int index = 2; index < argc; ++index)
        {
            if (!readServeOption(argc, argv, index, config))
            {
                printUsage(argv[0]);
                return 1;
            }
        }

        mark4::HubApp app(config);
        if (!app.init())
        {
            static_cast<void>(std::fprintf(stderr, "hub: initialization failed\n"));
            return 1;
        }
        installSignalHandlers(app);
        static_cast<void>(std::printf(
            "hub: watching announces on udp/%u, telemetry on udp/%u, sim raw on udp/%u\n",
            static_cast<unsigned>(config.announcePort),
            static_cast<unsigned>(config.telemetryPort),
            static_cast<unsigned>(config.simRawPort)));
        static_cast<void>(std::fflush(stdout));

        const int code = app.run(nullptr);
        G_APP.store(nullptr);
        const mark4::StreamRecorder::Stats &stats = app.accessRecorder().stats();
        static_cast<void>(std::printf(
            "hub: %llu telemetry rows, %llu sim raw rows, %llu blackbox records, %llu bad frames\n",
            static_cast<unsigned long long>(stats.telemetryRows),
            static_cast<unsigned long long>(stats.simRawRows),
            static_cast<unsigned long long>(stats.blackboxRecords),
            static_cast<unsigned long long>(stats.badFrames)));
        return code;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printUsage(argv[0]);
        return 1;
    }
    if (std::strcmp(argv[1], "serve") == 0)
    {
        return runServe(argc, argv);
    }
    printUsage(argv[0]);
    return 1;
}
