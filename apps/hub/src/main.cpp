/// @file
/// @brief hub entry point: parses the subcommand and its options, builds the
///        app, runs it.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include "hub/hub_app.hpp"
#include "hub/launcher.hpp"
#include "hub/tuning_profiles.hpp"

namespace
{
    /// Frame budget given to the flight process of a scenario: large enough
    /// that a session ends because the operator ended it, never because a
    /// counter ran out.
    constexpr std::uint32_t DEFAULT_SIM_FRAMES = 4'000'000'000U;

    /// Board UART a real flight uses unless told otherwise.
    constexpr const char *DEFAULT_SERIAL_DEVICE = "/dev/ttyUSB0";

    /// Delay left to the simulator to boot before the flight process starts.
    constexpr long GODOT_SETTLE_NS = 500'000'000L;

    /// Delay between two checks of the children when the hub serves nothing.
    constexpr long SUPERVISE_POLL_NS = 100'000'000L;

    /// App the signal handlers stop. A file-scope pointer is the only thing a
    /// POSIX handler can reach, and requestStop() is the only thing it calls:
    /// one atomic store, nothing else.
    std::atomic<mark4::HubApp *> G_APP{nullptr};

    /// Set by the stop handlers when no app is running, so a scenario the hub
    /// only supervises still ends on Ctrl-C.
    std::atomic_bool G_STOP_SUPERVISOR{false};

    /// @brief Stop handler of a scenario the hub only supervises.
    /// @param signalNumber signal received, unused
    void onSuperviseSignal(int signalNumber)
    {
        static_cast<void>(signalNumber);
        G_STOP_SUPERVISOR.store(true);
    }

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
            "usage: %s <subcommand> [options]\n"
            "\n"
            "  serve                 decode the streams, record them, serve the websocket\n"
            "  up sim                start the simulator pair, then serve\n"
            "  up real               own the board UART, then serve\n"
            "  up replay <log.m4bb>  replay a recorded flight, then serve\n"
            "  profile list          name the stored tuning profiles\n"
            "  profile show NAME     print one stored tuning profile\n"
            "\n"
            "scenario options:\n"
            "  --no-serve           supervise the children only, serve nothing\n"
            "  --godot PATH         godot 4 binary (default godot)\n"
            "  --drone-sim PATH     drone_sim binary\n"
            "  --drone-replay PATH  drone_replay binary\n"
            "  --headless           run godot without a window\n"
            "  --lockstep           run the simulator in lockstep with the flight process\n"
            "  --time-scale F       simulator speed factor\n"
            "  --arena-radius F     circular wall around the launch point [m]\n"
            "  --frames N           frame budget of the flight process\n"
            "  --sim-port N         sim link port (default %u)\n"
            "  --rc-port N          rc uplink port (default %u)\n"
            "  --speed F|max        replay speed\n"
            "\n"
            "serve options:\n"
            "  --ws-port N          websocket endpoint port (default %u)\n"
            "  --announce-port N    announce listen port (default %u)\n"
            "  --telemetry-port N   telemetry port watched by default (default %u)\n"
            "  --raw-port N         sim raw port watched by default (default %u)\n"
            "  --serial DEV         board UART to own, none by default\n"
            "  --baud N             board UART speed (default %u)\n"
            "  --record             open a CSV recording at startup\n"
            "  --log-dir DIR        directory recordings are written to (default logs)\n"
            "  --profiles PATH      directory tuning profiles live in (default profiles)\n"
            "  --push-profile NAME  push this profile to every process that appears\n",
            program,
            static_cast<unsigned>(mark4::SIM_LINK_PORT),
            static_cast<unsigned>(mark4::RC_COMMAND_PORT),
            static_cast<unsigned>(mark4::HubApp::WS_PORT),
            static_cast<unsigned>(mark4::ANNOUNCE_PORT),
            static_cast<unsigned>(mark4::TELEMETRY_PORT),
            static_cast<unsigned>(mark4::SIM_RAW_PORT),
            mark4::HubApp::DEFAULT_SERIAL_BAUD));
    }

    /// @brief Prints what the run recorded.
    /// @param app app that ran
    void reportCounters(const mark4::HubApp &app)
    {
        const mark4::StreamRecorder::Stats &stats = app.accessRecorder().stats();
        static_cast<void>(std::printf(
            "hub: %llu telemetry rows, %llu sim raw rows, %llu blackbox records, %llu bad frames\n",
            static_cast<unsigned long long>(stats.telemetryRows),
            static_cast<unsigned long long>(stats.simRawRows),
            static_cast<unsigned long long>(stats.blackboxRecords),
            static_cast<unsigned long long>(stats.badFrames)));
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
        if (std::strcmp(name, "--profiles") == 0 && hasValue)
        {
            config.profilesDir = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--push-profile") == 0 && hasValue)
        {
            config.pushProfileName = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--record") == 0)
        {
            config.recordOnStart = true;
            return true;
        }
        return false;
    }

    /// @brief Runs the profile subcommand family: plain file reads, no
    ///        socket and no running hub, so a bench session can look at what
    ///        it stored without starting anything.
    /// @param argc argument count
    /// @param argv argument values, argv[1] being the subcommand
    /// @return process exit code
    int runProfile(int argc, char **argv)
    {
        if (argc < 3)
        {
            printUsage(argv[0]);
            return 1;
        }
        // "show" takes a profile name of its own; the options start after
        // whatever the verb consumed.
        const bool show = std::strcmp(argv[2], "show") == 0;
        const bool list = std::strcmp(argv[2], "list") == 0;
        const int firstOption = show ? 4 : 3;
        if ((!show && !list) || argc < firstOption)
        {
            printUsage(argv[0]);
            return 1;
        }

        mark4::HubApp::Config config;
        for (int index = firstOption; index < argc; ++index)
        {
            if (!readServeOption(argc, argv, index, config))
            {
                printUsage(argv[0]);
                return 1;
            }
        }
        const mark4::TuningProfiles profiles(config.profilesDir);

        if (list)
        {
            for (const std::string &name : profiles.list())
            {
                static_cast<void>(std::printf("%s\n", name.c_str()));
            }
            return 0;
        }

        mark4::TuningValues values;
        std::string error;
        if (!profiles.load(argv[3], values, error))
        {
            static_cast<void>(std::fprintf(stderr, "hub: %s\n", error.c_str()));
            return 1;
        }
        static_cast<void>(std::printf("%s\n", mark4::profileToJson(argv[3], values).c_str()));
        return 0;
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
        reportCounters(app);
        return code;
    }

    /// Everything the scenario subcommands decide on top of the serve options.
    struct UpOptions
    {
        std::string godot = "godot";                   ///< godot 4 binary
        std::string droneSim;                          ///< drone_sim binary
        std::string droneReplay;                       ///< drone_replay binary
        std::string replayFile;                        ///< recording to replay
        std::string speed;                             ///< replay speed, empty = default
        std::string timeScale;                         ///< simulator speed factor, empty = default
        std::string arenaRadius;                       ///< arena radius [m], empty = default
        std::uint32_t frames = DEFAULT_SIM_FRAMES;     ///< frame budget of the flight process
        std::uint16_t simPort = mark4::SIM_LINK_PORT;  ///< sim link port
        std::uint16_t rcPort = mark4::RC_COMMAND_PORT; ///< rc uplink port
        bool rcPortGiven = false; ///< the rc port was named on the command line
        bool headless = false;    ///< run godot without a window
        bool lockstep = false;    ///< run the simulator in lockstep
        bool serve = true;        ///< run the hub next to the children
    };

    /// @brief Reads one scenario option.
    /// @param argc argument count
    /// @param argv argument values
    /// @param index index of the argument to read, advanced past what is consumed
    /// @param options options being filled
    /// @return true when the argument was one of these options and parsed
    bool readUpOption(int argc, char **argv, int &index, UpOptions &options)
    {
        const char *name = argv[index];
        const bool hasValue = index + 1 < argc;
        if (std::strcmp(name, "--godot") == 0 && hasValue)
        {
            options.godot = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--drone-sim") == 0 && hasValue)
        {
            options.droneSim = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--drone-replay") == 0 && hasValue)
        {
            options.droneReplay = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--speed") == 0 && hasValue)
        {
            options.speed = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--time-scale") == 0 && hasValue)
        {
            options.timeScale = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--arena-radius") == 0 && hasValue)
        {
            options.arenaRadius = argv[++index];
            return true;
        }
        if (std::strcmp(name, "--frames") == 0 && hasValue)
        {
            return parseUnsigned(argv[++index], options.frames);
        }
        if (std::strcmp(name, "--sim-port") == 0 && hasValue)
        {
            return parsePort(argv[++index], options.simPort);
        }
        if (std::strcmp(name, "--rc-port") == 0 && hasValue)
        {
            options.rcPortGiven = true;
            return parsePort(argv[++index], options.rcPort);
        }
        if (std::strcmp(name, "--headless") == 0)
        {
            options.headless = true;
            return true;
        }
        if (std::strcmp(name, "--lockstep") == 0)
        {
            options.lockstep = true;
            return true;
        }
        if (std::strcmp(name, "--no-serve") == 0)
        {
            options.serve = false;
            return true;
        }
        return false;
    }

    /// @brief Runs the hub next to the children, or just watches them.
    /// @param group children of the scenario
    /// @param config hub configuration
    /// @param serve true to run the hub, false to only supervise
    /// @return exit code: the children's when one of them stopped the run
    int superviseAndServe(mark4::ProcessGroup &group,
                          const mark4::HubApp::Config &config,
                          bool serve)
    {
        if (!serve)
        {
            G_STOP_SUPERVISOR.store(false);
            static_cast<void>(std::signal(SIGINT, onSuperviseSignal));
            static_cast<void>(std::signal(SIGTERM, onSuperviseSignal));
            while (!G_STOP_SUPERVISOR.load() && !group.anyExited())
            {
                timespec delay{};
                delay.tv_nsec = SUPERVISE_POLL_NS;
                static_cast<void>(::nanosleep(&delay, nullptr));
            }
            return group.terminateAll();
        }

        mark4::HubApp app(config);
        if (!app.init())
        {
            static_cast<void>(std::fprintf(stderr, "hub: initialization failed\n"));
            static_cast<void>(group.terminateAll());
            return 1;
        }
        installSignalHandlers(app);
        // The hub outlives nothing: when a child of the scenario goes, the
        // scenario is over and the whole group goes with it.
        static_cast<void>(app.run([&group]() { return !group.anyExited(); }));
        G_APP.store(nullptr);
        reportCounters(app);
        return group.terminateAll();
    }

    /// @brief Runs the up sim subcommand.
    /// @param argc argument count
    /// @param argv argument values
    /// @return process exit code
    int runUpSim(int argc, char **argv)
    {
        mark4::HubApp::Config config;
        UpOptions options;
        options.droneSim = mark4::defaultBinaryPath("drone_sim");
        for (int index = 3; index < argc; ++index)
        {
            if (!readServeOption(argc, argv, index, config) &&
                !readUpOption(argc, argv, index, options))
            {
                printUsage(argv[0]);
                return 1;
            }
        }

        const std::string project = mark4::defaultProjectPath("sim-godot");
        std::error_code failure;
        if (!std::filesystem::exists(project + "/.godot", failure))
        {
            // First launch of a fresh checkout: let Godot import its
            // resources before anything tries to run the scene.
            static_cast<void>(std::printf("hub: importing the godot project (first run)...\n"));
            static_cast<void>(std::fflush(stdout));
            mark4::ChildSpec import;
            import.program = options.godot;
            import.arguments = {"--headless", "--path", project, "--import"};
            int code = 0;
            if (!mark4::runChildToCompletion(import, code) || code != 0)
            {
                static_cast<void>(
                    std::fprintf(stderr, "hub: the godot import failed (code %d)\n", code));
                return 1;
            }
        }

        mark4::ChildSpec godot;
        godot.program = options.godot;
        if (options.headless)
        {
            godot.arguments.emplace_back("--headless");
        }
        godot.arguments.emplace_back("--path");
        godot.arguments.push_back(project);
        godot.arguments.emplace_back("--");
        godot.arguments.emplace_back("--flight-port");
        godot.arguments.push_back(std::to_string(options.simPort));
        godot.arguments.emplace_back("--raw-port");
        godot.arguments.push_back(std::to_string(config.simRawPort));
        godot.arguments.emplace_back("--rc-port");
        godot.arguments.push_back(std::to_string(options.rcPort));
        if (options.lockstep)
        {
            godot.arguments.emplace_back("--lockstep");
        }
        if (!options.timeScale.empty())
        {
            godot.arguments.emplace_back("--time-scale");
            godot.arguments.push_back(options.timeScale);
        }
        if (!options.arenaRadius.empty())
        {
            godot.arguments.emplace_back("--arena-radius");
            godot.arguments.push_back(options.arenaRadius);
        }

        mark4::ChildSpec droneSim;
        droneSim.program = options.droneSim;
        droneSim.arguments.push_back(std::to_string(options.frames));
        droneSim.arguments.emplace_back("--sim-port");
        droneSim.arguments.push_back(std::to_string(options.simPort));
        droneSim.arguments.emplace_back("--telemetry-port");
        droneSim.arguments.push_back(std::to_string(config.telemetryPort));
        if (options.rcPortGiven)
        {
            // Only when asked for: a flight process that does not know the
            // option yet refuses the whole command line over it.
            droneSim.arguments.emplace_back("--rc-port");
            droneSim.arguments.push_back(std::to_string(options.rcPort));
        }

        mark4::ProcessGroup group;
        // Godot first: it resends until the flight process answers, while the
        // flight process would give up waiting for a slow Godot boot.
        if (!group.spawn(godot))
        {
            return 1;
        }
        timespec settle{};
        settle.tv_nsec = GODOT_SETTLE_NS;
        static_cast<void>(::nanosleep(&settle, nullptr));
        if (!group.spawn(droneSim))
        {
            static_cast<void>(group.terminateAll());
            return 1;
        }
        return superviseAndServe(group, config, options.serve);
    }

    /// @brief Runs the up real subcommand.
    /// @param argc argument count
    /// @param argv argument values
    /// @return process exit code
    int runUpReal(int argc, char **argv)
    {
        mark4::HubApp::Config config;
        config.serialDevice = DEFAULT_SERIAL_DEVICE;
        UpOptions options;
        for (int index = 3; index < argc; ++index)
        {
            if (!readServeOption(argc, argv, index, config) &&
                !readUpOption(argc, argv, index, options))
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
        if (!app.accessSerial().isOpen())
        {
            static_cast<void>(std::fprintf(stderr,
                                           "hub: %s is not usable, and a real flight needs it\n",
                                           config.serialDevice.c_str()));
            return 1;
        }
        installSignalHandlers(app);
        static_cast<void>(std::printf("hub: owning %s at %u baud, websocket on tcp/%u\n",
                                      config.serialDevice.c_str(),
                                      config.serialBaud,
                                      static_cast<unsigned>(config.wsPort)));
        static_cast<void>(std::fflush(stdout));

        const int code = app.run(nullptr);
        G_APP.store(nullptr);
        reportCounters(app);
        return code;
    }

    /// @brief Runs the up replay subcommand.
    /// @param argc argument count
    /// @param argv argument values
    /// @return process exit code
    int runUpReplay(int argc, char **argv)
    {
        if (argc < 4)
        {
            printUsage(argv[0]);
            return 1;
        }
        mark4::HubApp::Config config;
        UpOptions options;
        options.droneReplay = mark4::defaultBinaryPath("drone_replay");
        options.replayFile = argv[3];
        for (int index = 4; index < argc; ++index)
        {
            if (!readServeOption(argc, argv, index, config) &&
                !readUpOption(argc, argv, index, options))
            {
                printUsage(argv[0]);
                return 1;
            }
        }

        mark4::ChildSpec replay;
        replay.program = options.droneReplay;
        replay.arguments.push_back(options.replayFile);
        if (!options.speed.empty())
        {
            replay.arguments.emplace_back("--speed");
            replay.arguments.push_back(options.speed);
        }

        mark4::ProcessGroup group;
        if (!group.spawn(replay))
        {
            return 1;
        }
        return superviseAndServe(group, config, options.serve);
    }

    /// @brief Runs the up subcommand family.
    /// @param argc argument count
    /// @param argv argument values
    /// @return process exit code
    int runUp(int argc, char **argv)
    {
        if (argc < 3)
        {
            printUsage(argv[0]);
            return 1;
        }
        if (std::strcmp(argv[2], "sim") == 0)
        {
            return runUpSim(argc, argv);
        }
        if (std::strcmp(argv[2], "real") == 0)
        {
            return runUpReal(argc, argv);
        }
        if (std::strcmp(argv[2], "replay") == 0)
        {
            return runUpReplay(argc, argv);
        }
        printUsage(argv[0]);
        return 1;
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
    if (std::strcmp(argv[1], "up") == 0)
    {
        return runUp(argc, argv);
    }
    if (std::strcmp(argv[1], "profile") == 0)
    {
        return runProfile(argc, argv);
    }
    printUsage(argv[0]);
    return 1;
}
