#pragma once

/// @file
/// @brief hub composition root: the gateway between the transport and the
///        websocket clients.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>

#include "discovery/discovery_directory.hpp"
#include "gateway.pb.h"
#include "hub/gateway_codec.hpp"
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
    /// The gateway forwards and does not interpret: every payload the
    /// transport delivers goes to the clients as a Frame from the
    /// messenger's raw tap, every Frame a client sends goes out on the
    /// transport. What it knows for itself it knows through its consumers,
    /// one per concept: they name the kinds that carry each provider,
    /// subscribe, pull the tables and drop everything of a node that goes
    /// down, and the gateway listens to them. The update consumer is fed by
    /// a handler of its own. It is a node too: its own log lines go out
    /// through its LogProvider, whose local sink mirrors them to the
    /// clients as frames from itself.
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

      private:
        /// @brief Mirrors one payload the transport delivered to the clients,
        ///        raw, before the messenger decodes it.
        /// @param src transport node the payload came from
        /// @param data payload bytes
        /// @param size payload size
        void mirror(std::uint32_t src, const std::uint8_t *data, std::size_t size);

        /// @brief The messenger's raw tap, forwards to mirror().
        static void Tap(void *context,
                        std::uint32_t src,
                        const std::uint8_t *data,
                        std::size_t size);

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

        /// What the gateway hears of a Status consumer. Nothing yet: the
        /// reports reach the clients through the raw mirror.
        class StatusEars final : public AbsStatusConsumerListener
        {
          public:
            /// @param consumer consumer to listen to
            /// @param app the gateway the events act on
            StatusEars(StatusConsumerBase &consumer, HubApp &app)
                : AbsStatusConsumerListener(consumer),
                  m_app(app)
            {
            }

            void onStatus(std::uint32_t nodeId,
                          const mark4_Status &status,
                          std::uint64_t nowUs) override;
            void onForgotten(std::uint32_t nodeId) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// What the gateway hears of its log consumer: a module table that
        /// changed is a node table to publish again.
        class LogEars final : public AbsLogConsumerListener
        {
          public:
            /// @param consumer consumer to listen to
            /// @param app the gateway the events act on
            LogEars(LogConsumerBase &consumer, HubApp &app)
                : AbsLogConsumerListener(consumer),
                  m_app(app)
            {
            }

            void onModules(std::uint32_t nodeId,
                           std::span<const mark4_LogModuleInfo> modules) override;
            void onLine(std::uint32_t nodeId, const mark4_Log &line) override;
            void onForgotten(std::uint32_t nodeId) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// What the gateway hears of its telemetry consumer: a table that
        /// arrived, or a node that took it away, is published at once.
        class TelemetryEars final : public AbsTelemetryConsumerListener
        {
          public:
            /// @param consumer consumer to listen to
            /// @param app the gateway the events act on
            TelemetryEars(TelemetryConsumerBase &consumer, HubApp &app)
                : AbsTelemetryConsumerListener(consumer),
                  m_app(app)
            {
            }

            void onTable(std::uint32_t nodeId,
                         std::span<const mark4_TelemetryDescriptor> descriptors) override;
            void onConfig(std::uint32_t nodeId,
                          const mark4_TelemetryConfig &config,
                          bool subscribed) override;
            void onSamples(std::uint32_t nodeId, const mark4_TelemetryData &data) override;
            void onForgotten(std::uint32_t nodeId) override;

          private:
            HubApp &m_app; ///< the gateway
        };

        /// What the gateway hears of its tuning consumer. Nothing yet: the
        /// answers reach the clients through the raw mirror.
        class TuningEars final : public AbsTuningConsumerListener
        {
          public:
            /// @param consumer consumer to listen to
            /// @param app the gateway the events act on
            TuningEars(TuningConsumerBase &consumer, HubApp &app)
                : AbsTuningConsumerListener(consumer),
                  m_app(app)
            {
            }

            void onTable(std::uint32_t nodeId, std::span<const mark4_TuningInfo> infos) override;
            void onResult(std::uint32_t nodeId, const mark4_TuningAck &ack) override;
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

        /// The gateway's own lines: a messenger refuses this node as a
        /// destination, so the provider feeds them here instead and they
        /// reach the clients as a frame from this node, exactly like every
        /// other node's.
        class OwnLogMirror final : public AbsLogSink
        {
          public:
            /// @param app the gateway the lines are mirrored through
            explicit OwnLogMirror(HubApp &app)
                : m_app(app)
            {
            }

            void write(const LogRecord &record) override;

          private:
            HubApp &m_app; ///< the gateway
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

        /// @brief Sends one message to every client.
        void broadcast(const mark4_GatewayMessage &message);

        /// @brief Sends the node table, the gateway itself first.
        void broadcastNodes();

        /// @brief Sends one node's telemetry table to every client.
        /// @param node node the table belongs to
        void broadcastNodeTelemetry(std::uint32_t node);

        /// @brief Sends the gateway counters.
        void broadcastStatus();

        /// @brief Sends the state of the update client, on every change.
        void broadcastOta();

        /// @brief Timers, update client tick, fresh snapshots for newcomers.
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
        StatusEars m_statusEars{m_status, *this};                ///< what Status means here
        LogEars m_logEars{m_logs, *this};                        ///< what a module table means
        TelemetryEars m_telemetryEars{m_telemetryTables, *this}; ///< what a measure table means
        TuningEars m_tuningEars{m_tuning, *this};                ///< what a parameter means
        OtaReader m_otaReader{m_messenger, *this};               ///< the update answers
        ConsoleSinkPosix m_consoleSink;                          ///< log lines on stdout
        /// This node's log on the wire: the lines to whoever subscribed, the
        /// module table one page per request, the levels.
        LogProvider m_logProvider{m_messenger};
        OwnLogMirror m_logMirror{*this};                 ///< its own lines, towards the clients
        WsBridge m_ws;                                   ///< websocket endpoint
        OtaConsumer m_ota;                               ///< firmware update session
        std::uint32_t m_otaTarget = 0U;                  ///< node the updater talks to
        std::map<std::string, std::uint64_t> m_rcSeenUs; ///< last RC instant per client
        std::atomic_bool m_stopRequested{false};         ///< set by a signal handler
        std::uint64_t m_nextStatusUs = 0U;               ///< next periodic publish [us]
        bool m_nodesDirty = false;                       ///< table changed since published
        bool m_loopbackWarned = false;                   ///< the link's fallback was logged
        std::uint32_t m_framesIn = 0U;                   ///< payloads delivered by the transport
        std::uint32_t m_framesOut = 0U;                  ///< frames sent for clients
        std::uint32_t m_badFrames = 0U;                  ///< client frames refused
    };
} // namespace mark4
