/// @file
/// @brief hub entry point. The hub takes no arguments: it serves with its
///        built-in defaults and everything operational (the board link, the
///        tuning profiles) is driven at runtime through the websocket by the
///        pages. Anything worth deciding earlier than that is a compile-time
///        default in transport/udp_link.hpp or HubApp::Config.

#include <atomic>
#include <csignal>
#include <cstdio>

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
} // namespace

int main(int argc, char **argv)
{
    static_cast<void>(argv);
    if (argc > 1)
    {
        static_cast<void>(std::fprintf(
            stderr,
            "hub takes no arguments: it serves on tcp/%u and is driven from the pages\n",
            static_cast<unsigned>(mark4::HubApp::WS_PORT)));
        return 1;
    }

    mark4::HubApp app{mark4::HubApp::Config{}};
    if (!app.init())
    {
        static_cast<void>(std::fprintf(stderr, "hub: initialization failed\n"));
        return 1;
    }
    G_APP.store(&app);
    static_cast<void>(std::signal(SIGINT, onStopSignal));
    static_cast<void>(std::signal(SIGTERM, onStopSignal));

    static_cast<void>(std::printf("hub: pages and websocket on http://127.0.0.1:%u\n",
                                  static_cast<unsigned>(mark4::HubApp::WS_PORT)));
    static_cast<void>(std::fflush(stdout));

    const int code = app.run(nullptr);
    G_APP.store(nullptr);
    return code;
}
