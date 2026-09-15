#pragma once

/// @file
/// @brief hub composition root: the gateway between the transport and the
///        websocket clients.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "discovery/discovery_directory.hpp"
#include "gateway.pb.h"
#include "hub/gateway_codec.hpp"
#include "hub/gateway_log.hpp"
#include "hub/gateway_pilot.hpp"
#include "hub/gateway_publisher.hpp"
#include "hub/gateway_status.hpp"
#include "hub/gateway_telemetry.hpp"
#include "hub/gateway_tuning.hpp"
#include "hub/tuning_profiles.hpp"
#include "hub/ws_bridge.hpp"
#include "log/console_sink_posix.hpp"
#include "log/consumer.hpp"
#include "log/provider.hpp"
#include "messaging/messenger.hpp"
#include "ota/consumer.hpp"
#include "protocol/envelope.hpp"
#include "status/consumer.hpp"
#include "telemetry/consumer.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"
#include "tuning/consumer.hpp"

namespace mark4
{
    /// Composition root: owns every service as a value member. Member
    /// declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no
    /// manual teardown. Built by main(), passed by reference: no singleton.
    ///
    /// Nothing raw crosses this gateway: what it knows it knows through its
    /// consumers, one per concept (they name the kinds that carry each
    /// provider, subscribe, pull the tables and drop everything of a node
    /// that goes down), and one gateway per concept publishes what its
    /// consumer holds as the typed messages of gateway.proto. A client
    /// sends typed commands, which are routed to the gateway of their
    /// concept. The update consumer is fed by a handler of its own. It is a
    /// node too: its own log lines reach its clients through the local sink
    /// of its LogProvider, which is the log gateway.
    class HubApp : public AbsGatewayPublisher
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

        /// Everything main() decides before the hub starts.
        struct Config
        {
            std::uint16_t wsPort = WS_PORT;               ///< websocket endpoint port
            std::uint16_t discoveryPort = DISCOVERY_PORT; ///< shared transport port
            std::uint32_t nodeId = 0U;                    ///< transport identity, 0 = random
            std::string profilesDir = "profiles";         ///< directory the profiles live in
            std::string telemetryDir;                     ///< directory the CSV exports and the
                                                          ///< named view configs live in, empty =
                                                          ///< the built-in default resolved at init
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

        /// Directory the telemetry exports and view configs are stored in
        /// when nothing else is asked for, relative to the source tree root.
        /// Under logs/, which the repository ignores: an export is bench
        /// output, not source.
        static constexpr const char *DEFAULT_TELEMETRY_DIR = "logs/telemetry";

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

        /// @brief Sends one message to every client.
        /// @param message message to publish
        void broadcast(const mark4_GatewayMessage &message) override;

        /// @brief Sends one message to one client.
        /// @param clientId connection to reach
        /// @param message message to publish
        void sendTo(const std::string &clientId, const mark4_GatewayMessage &message) override;

      private:
        /// The gateway's ear on the transport's node table: it logs what
        /// comes and goes and marks the table dirty. What a node's arrival
        /// and departure mean for the concepts is the consumers' business.
        class PresenceListener final : public AbsPresenceListener
        {
          public:
            /// @param transport transport to listen to
            /// @param app the gateway the events act on
            PresenceListener(Transport &transport, HubApp &app)
                : AbsPresenceListener(transport),
                  m_app(app)
            {
            }

            void onNodeUp(const Transport::Node &node) override;
            void onNodeDown(const Transport::Node &node) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// The gateway's ear on its directory: a node whose identity is
        /// learnt goes into the node table. What that identity means for
        /// each concept is the consumers' business; they listen to the same
        /// directory.
        class IdentityListener final : public AbsDirectoryListener
        {
          public:
            /// @param directory directory to listen to
            /// @param app the gateway the events act on
            IdentityListener(DiscoveryDirectory &directory, HubApp &app)
                : AbsDirectoryListener(directory),
                  m_app(app)
            {
            }

            void onIdentity(const DirectoryEntry &entry) override;
            void onForgotten(std::uint32_t nodeId) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// The update consumer's ear on the wire. It is the one consumer
        /// that is not an AbsMessageHandler of its own (it was moved as it
        /// was), so the gateway hands it what it answers to.
        class OtaReader final : public AbsMessageHandler
        {
          public:
            /// Body tags this handler consumes.
            static constexpr std::array<pb_size_t, 3> TAGS = {mark4_Envelope_ota_status_tag,
                                                              mark4_Envelope_ota_ack_tag,
                                                              mark4_Envelope_ota_chunk_ack_tag};

            /// @param messenger messenger to attach to
            /// @param app the gateway the messages act on
            OtaReader(Messenger &messenger, HubApp &app)
                : AbsMessageHandler(messenger, TAGS),
                  m_app(app)
            {
            }

            bool onMessage(std::uint32_t src,
                           const mark4_Envelope &envelope,
                           std::uint64_t nowUs) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// What a node command travels as: a request is numbered, kept and
        /// resent until the node acknowledges it, and only a handler owns
        /// requests. This one claims no tag, because what it sends (a
        /// reboot, a scenario) is answered by nothing but the
        /// acknowledgement the messenger itself reads.
        class HubRequester final : public AbsMessageHandler
        {
          public:
            /// @param messenger messenger to attach to
            explicit HubRequester(Messenger &messenger)
                : AbsMessageHandler(messenger, {})
            {
            }

            /// @brief Nothing is dispatched here: this handler claims no tag.
            /// @param src node it came from
            /// @param envelope the message
            /// @param nowUs instant of the poll that delivered it [us]
            /// @return false, always
            bool onMessage(std::uint32_t src,
                           const mark4_Envelope &envelope,
                           std::uint64_t nowUs) override;

            /// @brief Sends one message as a request of this handler's.
            /// @param dst node to reach
            /// @param envelope message to send
            /// @return true when the request was taken
            bool ask(std::uint32_t dst, mark4_Envelope &envelope);
        };

        /// @brief Clock the log records are stamped with.
        static std::uint64_t LogClock(void *context);

        /// @brief Sends one envelope to one node through the messenger.
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

        /// @brief Carries out one node command: a reboot, a scenario.
        bool applyNodeCommand(const mark4_NodeCommand &command, std::string &errorOut);

        /// @return the node table, the gateway itself first
        mark4_GatewayMessage nodesMessage();

        /// @return the gateway counters
        mark4_GatewayMessage statusMessage();

        /// @return the state of the update client
        mark4_GatewayMessage otaMessage();

        /// @brief Sends everything the gateway holds to a client that just
        ///        connected, that client alone.
        /// @param clientId the client
        void snapshot(const std::string &clientId);

        /// @brief Timers, update client tick, snapshots for newcomers.
        /// @param nowUs current time [us]
        void housekeeping(std::uint64_t nowUs);

        Config m_config;           ///< settings of this run
        TuningProfiles m_profiles; ///< stored tuning profiles
        UdpLink m_udpLink;         ///< the LAN link, the only one
        /// This gateway's identity: the first row of the node table it
        /// publishes, and what its directory answers to whoever asks.
        /// Complete at construction, before the directory copies it.
        mark4_Announce
            m_ownAnnounce; ///< this gateway's identity: the directory's self, row 0 of the table
        Transport m_transport; ///< this hub as a transport node
        /// The requests waiting for their acknowledgement, owned here and
        /// handed to the messenger as a span: one node appearing costs
        /// several requests at once, and a gateway watches a whole LAN.
        std::array<PendingRequest, Messenger::HUB_PENDING_REQUESTS> m_pendingRequests{};
        Messenger m_messenger{m_transport, m_pendingRequests}; ///< decodes for the handlers below
        PresenceListener m_presence{m_transport, *this};       ///< its node table events
        /// Who is who: asks every node that appears, keeps the answers.
        DiscoveryDirectory m_directory{m_messenger, m_transport, m_ownAnnounce}; ///< who is who
        IdentityListener m_identities{m_directory, *this}; ///< what the directory learns
        /// What the gateway keeps of each concept: one consumer per concept,
        /// each sized for every node the transport can hold.
        StatusConsumer<Transport::MAX_NODES> m_status{m_messenger, m_directory};
        LogConsumer<Transport::MAX_NODES> m_logs{m_messenger, m_directory};    ///< module tables
        TelemetryConsumer<Transport::MAX_NODES> m_telemetryTables{m_messenger, ///< measure tables
                                                                  m_directory};
        TuningConsumer<Transport::MAX_NODES> m_tuning{m_messenger, m_directory}; ///< parameters
        OtaReader m_otaReader{m_messenger, *this}; ///< the update answers
        HubRequester m_requester{m_messenger};     ///< what this node asks of another
        ConsoleSinkPosix m_consoleSink;            ///< log lines on stdout
        /// This node's log on the wire: the lines to whoever subscribed, the
        /// module table one page per request, the levels.
        LogProvider m_logProvider{m_messenger};
        WsBridge m_ws;                  ///< websocket endpoint
        OtaConsumer m_ota;              ///< firmware update session
        std::uint32_t m_otaTarget = 0U; ///< node the updater talks to
        /// One gateway per concept: it listens to its consumer, publishes
        /// what it holds and carries the clients' commands out through it.
        StatusGateway m_statusGateway{*this, m_status};
        LogGateway m_logGateway{*this, m_logs, m_transport.nodeId()};  ///< lines and levels
        TelemetryGateway m_telemetryGateway{*this, m_telemetryTables}; ///< tables and samples
        TuningGateway m_tuningGateway{*this, m_tuning};                ///< parameters
        PilotGateway m_pilotGateway{m_messenger};                      ///< the pilot seats
        std::atomic_bool m_stopRequested{false};                       ///< set by a signal
        std::uint64_t m_nextStatusUs = 0U;                             ///< next publish [us]
        bool m_nodesDirty = false;                                     ///< table changed
        bool m_loopbackWarned = false;        ///< the link's fallback was logged
        std::uint32_t m_commands = 0U;        ///< client commands carried out
        std::uint32_t m_refusedCommands = 0U; ///< client commands refused
    };
} // namespace mark4
