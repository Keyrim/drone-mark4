#pragma once

/// @file
/// @brief hub composition root: the gateway between the transport and the
///        websocket clients.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "gateway.pb.h"
#include "hub/gateway_codec.hpp"
#include "hub/ota_client.hpp"
#include "hub/tuning_profiles.hpp"
#include "hub/ws_bridge.hpp"
#include "log/console_sink_posix.hpp"
#include "log/wire.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    /// Composition root: owns every service as a value member. Member
    /// declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no
    /// manual teardown. Built by main(), passed by reference: no singleton.
    ///
    /// The gateway forwards and does not interpret: every payload the
    /// transport delivers goes to the clients as a Frame, every Frame a
    /// client sends goes out on the transport. It decodes envelopes for two
    /// things only: to remember each node's last Announce and LogModules
    /// table (the node table) and to feed the update client its answers.
    /// It is a node too: its own log lines leave as Log envelopes on the
    /// transport and are mirrored to the clients as frames from itself.
    class HubApp
    {
      public:
        /// TCP port the websocket endpoint listens on. Deliberately not
        /// next to the transport's DISCOVERY_PORT: that one is a UDP
        /// boundary between processes speaking the binary wire, this is
        /// neither.
        static constexpr std::uint16_t WS_PORT = 47810U;

        /// How long the poll loop sleeps when nothing is happening [ms].
        static constexpr int POLL_TIMEOUT_MS = 20;

        /// How long it sleeps while a firmware update runs [ms]. The chunk
        /// sender is paced from the loop, so the loop has to come round often
        /// enough for that pacing to be the throttle rather than the sleep.
        static constexpr int OTA_POLL_TIMEOUT_MS = 1;

        /// Period of the status and node table messages [ms].
        static constexpr std::uint64_t STATUS_PERIOD_MS = 1000U;

        /// A client whose last RC frame is older than this no longer counts
        /// as a pilot (the stream runs at 10 Hz while engaged).
        static constexpr std::uint64_t RC_PILOT_WINDOW_US = 2'000'000U;

        /// How long a telemetry page request goes unanswered before it is
        /// sent again [us]. A flight process answers from its command drain,
        /// so a lost page costs one frame; this is a lost datagram, not a
        /// slow node.
        static constexpr std::uint64_t TELEMETRY_RETRY_US = 500'000U;

        /// Requests for the same page before the node is given up on. Six
        /// tries at half a second is three seconds, the transport's own node
        /// expiry: past that the node is not answering, not merely slow.
        static constexpr std::uint32_t TELEMETRY_MAX_ATTEMPTS = 6U;

        /// Everything main() decides before the hub starts.
        struct Config
        {
            std::uint16_t wsPort = WS_PORT;               ///< websocket endpoint port
            std::uint16_t discoveryPort = DISCOVERY_PORT; ///< shared transport port
            std::uint32_t nodeId = 0U;                    ///< transport identity, 0 = random
            std::string profilesDir = "profiles";         ///< directory the profiles live in
            std::string pagesDir;                         ///< directory the static pages are
                                                          ///< read from, empty = the built-in
                                                          ///< default resolved at init
            std::string bindAddress = "127.0.0.1";        ///< address the endpoint binds to
            std::string otaBundlePath;                    ///< bundle an update with no path of
                                                          ///< its own sends, empty = the
                                                          ///< built-in default resolved at init
        };

        /// Directory the pages are read from when nothing else is asked for,
        /// relative to the source tree root.
        static constexpr const char *DEFAULT_PAGES_DIR = "software/hub/pages/dist";

        /// Bundle an update starts from when the client names none: the
        /// standard output of the firmware build, relative to the source tree
        /// root. The file need not exist; nothing reads it before a start.
        static constexpr const char *DEFAULT_OTA_BUNDLE =
            "software/build/stm32/drone_firmware/drone_firmware.ota";

        /// @param config settings of this run
        explicit HubApp(Config config);

        /// @brief Initializes services in declaration order: the transport,
        ///        then the websocket. The first failure is logged and returns
        ///        false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the poll loop until a stop is requested or keepRunning
        ///        returns false.
        /// @param keepRunning polled once per loop; returning false ends the run
        /// @return process exit code
        int run(const std::function<bool()> &keepRunning);

        /// @brief Asks the loop to stop. Signal-safe: sets one atomic flag.
        void requestStop()
        {
            m_stopRequested.store(true);
        }

      private:
        /// @brief Handles one payload the transport delivered: forwards it
        ///        to the clients, remembers an Announce, feeds the updater.
        /// @param src transport node the payload came from
        /// @param data payload bytes
        /// @param size payload size
        void onFrame(std::uint32_t src, const std::uint8_t *data, std::size_t size);

        /// @brief Transport delivery callback, forwards to onFrame().
        static void OnFrame(void *context,
                            std::uint32_t src,
                            const std::uint8_t *data,
                            std::size_t size);

        /// @brief Transport presence callbacks: the table changed.
        static void OnNodeUp(void *context, const Transport::Node &node);
        static void OnNodeDown(void *context, const Transport::Node &node);

        /// @brief Route of the gateway's own log lines and module table: a
        ///        transport broadcast, mirrored to the clients as a frame
        ///        from this node (the transport never hands a node its own
        ///        broadcasts back).
        static bool SendLog(void *context, const std::uint8_t *data, std::size_t size);

        /// @brief Clock the log records are stamped with.
        static std::uint64_t LogClock(void *context);

        /// @brief Publishes this node's module table (LogModules pages).
        void publishLogModules();

        /// @brief Sends one envelope to one node as a transport unicast.
        /// @param dst node to reach
        /// @param envelope message to send
        /// @param errorOut receives the reason when it cannot go out
        /// @return true when the bytes went out
        bool sendEnvelope(std::uint32_t dst, const mark4_Envelope &envelope, std::string &errorOut);

        /// @brief Decodes and carries out everything the clients have sent.
        void handleClientMessages();

        /// @brief Carries out one decoded client message.
        /// @param message the message
        /// @param clientId connection it came from
        /// @param errorOut receives the refusal reason
        /// @return true when it was carried out
        bool applyClientMessage(const mark4_GatewayMessage &message,
                                const std::string &clientId,
                                std::string &errorOut);

        /// @brief Carries out one profile command.
        bool applyProfileCommand(const mark4_ProfileCommand &command, std::string &errorOut);

        /// @brief Sends one message to every client.
        void broadcast(const mark4_GatewayMessage &message);

        /// @brief Sends the node table, the gateway itself first.
        void broadcastNodes();

        /// @brief Sends one node's telemetry table to every client.
        /// @param node node the table belongs to
        void broadcastNodeTelemetry(std::uint32_t node);

        /// @brief Starts pulling a node's telemetry table, page by page. A
        ///        node already being pulled, or already pulled, is left
        ///        alone.
        /// @param node node to ask
        /// @param nowUs current time [us]
        void beginTelemetryPull(std::uint32_t node, std::uint64_t nowUs);

        /// @brief Asks one node for the page its pull is waiting on.
        /// @param node node to ask
        /// @param nowUs current time [us]
        void requestTelemetryPage(std::uint32_t node, std::uint64_t nowUs);

        /// @brief Merges one page into a node's table and asks for the next,
        ///        or publishes the table when it is whole.
        /// @param node node the page came from
        /// @param page the page
        /// @param nowUs current time [us]
        void onTelemetryPage(std::uint32_t node,
                             const mark4_TelemetryDescriptors &page,
                             std::uint64_t nowUs);

        /// @brief Re-sends the page requests that went unanswered, and gives
        ///        up on the nodes that never answer.
        /// @param nowUs current time [us]
        void pumpTelemetryPulls(std::uint64_t nowUs);

        /// @brief Sends the gateway counters.
        void broadcastStatus();

        /// @brief Sends the state of the update client, on every change.
        void broadcastOta();

        /// @brief Timers, update client tick, fresh snapshots for newcomers.
        /// @param nowUs current time [us]
        void housekeeping(std::uint64_t nowUs);

        Config m_config;                                         ///< settings of this run
        TuningProfiles m_profiles;                               ///< stored tuning profiles
        UdpLink m_udpLink;                                       ///< the LAN link, the only one
        Transport m_transport;                                   ///< this hub as a transport node
        ConsoleSinkPosix m_consoleSink;                          ///< log lines on stdout
        TransportSink m_logSink{&HubApp::SendLog, this};         ///< log lines on the wire
        WsBridge m_ws;                                           ///< websocket endpoint
        OtaClient m_ota;                                         ///< firmware update session
        std::uint32_t m_otaTarget = 0U;                          ///< node the updater talks to
        mark4_Announce m_ownAnnounce = mark4_Announce_init_zero; ///< this gateway's beacon
        std::map<std::uint32_t, mark4_Announce> m_announces;     ///< last beacon per node
        std::map<std::uint32_t, LogModuleTable> m_logModules;    ///< last module table per node

        /// Where one node's telemetry table stands: the descriptors pulled
        /// so far and what the walk is still waiting on.
        struct TelemetryPull
        {
            TelemetryTable table;             ///< descriptors merged so far
            std::uint32_t cursor = 0U;        ///< page the pull is waiting on
            std::uint64_t lastRequestUs = 0U; ///< instant the request went out [us]
            std::uint32_t attempts = 0U;      ///< requests sent for this page
            bool complete = false;            ///< the whole table arrived
            bool abandoned = false;           ///< the node never answered
        };
        std::map<std::uint32_t, TelemetryPull> m_telemetry; ///< one pull per drone node
        std::map<std::string, std::uint64_t> m_rcSeenUs;    ///< last RC instant per client
        std::atomic_bool m_stopRequested{false};            ///< set by a signal handler
        std::uint64_t m_nextStatusUs = 0U;                  ///< next periodic publish [us]
        bool m_nodesDirty = false;                          ///< table changed since published
        bool m_logModulesPublished = false; ///< own table sent after the first beacon
        bool m_loopbackWarned = false;      ///< the link's fallback was logged
        std::uint32_t m_framesIn = 0U;      ///< payloads delivered by the transport
        std::uint32_t m_framesOut = 0U;     ///< frames sent for clients
        std::uint32_t m_badFrames = 0U;     ///< client frames refused
    };
} // namespace mark4
