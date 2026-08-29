#pragma once

/// @file
/// @brief hub composition root.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hub/discovery.hpp"
#include "hub/json_codec.hpp"
#include "hub/ota_client.hpp"
#include "hub/tuning_profiles.hpp"
#include "hub/ws_bridge.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    /// Composition root: owns every service as a value member. Member
    /// declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no
    /// manual teardown. Built by main(), passed by reference: no singleton.
    class HubApp
    {
      public:
        /// TCP port the websocket endpoint listens on. Deliberately not in
        /// protocol/ports.hpp: that file describes the UDP boundaries between
        /// processes speaking the binary wire, and this is neither.
        static constexpr std::uint16_t WS_PORT = 47810U;

        /// How long the poll loop sleeps when nothing is happening [ms].
        static constexpr int POLL_TIMEOUT_MS = 20;

        /// How long it sleeps while a firmware update runs [ms]. The chunk
        /// sender is paced from the loop, so the loop has to come round often
        /// enough for that pacing to be the throttle rather than the sleep.
        static constexpr int OTA_POLL_TIMEOUT_MS = 1;

        /// Silence after which a discovered process is declared gone [us].
        /// The beacon cadence is one per second, first one immediate, so
        /// this is three missed beacons, the same figure the transport uses
        /// for its own node table.
        static constexpr std::uint64_t DISCOVERY_EXPIRY_US = 3'000'000U;

        /// Period of the status message [ms].
        static constexpr std::uint64_t STATUS_PERIOD_MS = 1000U;

        /// A client whose last RC message is older than this no longer
        /// counts as a pilot (the stream runs at 10 Hz while engaged).
        static constexpr std::uint64_t RC_PILOT_WINDOW_US = 2'000'000U;

        /// Highest scenario sequence number the hub stamps before wrapping.
        /// Zero is reserved on the wire for "no scenario", so the counter
        /// runs 1..255.
        static constexpr std::uint32_t MAX_SCENARIO_SEQUENCE = 255U;

        /// Everything main() decides before the hub starts.
        struct Config
        {
            std::uint16_t wsPort = WS_PORT;               ///< websocket endpoint port
            std::uint16_t discoveryPort = DISCOVERY_PORT; ///< shared transport port
            std::uint32_t nodeId = 0U;                    ///< transport identity, 0 = random
            std::string profilesDir = "profiles";         ///< directory the profiles live in
            std::string pushProfileName;                  ///< profile pushed to every process
                                                          ///< that appears, empty = none
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
        /// @brief Handles one payload the transport delivered.
        /// @param src transport node the payload came from
        /// @param data payload bytes
        /// @param size payload size
        void onFrame(std::uint32_t src, const std::uint8_t *data, std::size_t size);

        /// @brief Transport delivery callback, forwards to onFrame().
        /// @param context the HubApp
        /// @param src transport node the payload came from
        /// @param data payload bytes
        /// @param size payload size
        static void OnFrame(void *context,
                            std::uint32_t src,
                            const std::uint8_t *data,
                            std::size_t size);

        /// @brief Routes one decoded message.
        /// @param envelope decoded message
        /// @param nodeId transport node it came from
        /// @param kind kind of the sender, when discovery knows it
        void onEnvelope(const mark4_Envelope &envelope, std::uint32_t nodeId, mark4_NodeKind kind);

        /// @brief Sends one command to a process: a transport unicast to the
        ///        node whose beacon announced that kind, whatever link it
        ///        was heard on.
        /// @param target kind of process to reach
        /// @param envelope message to send
        /// @param errorOut receives the reason when the target is unreachable
        /// @return true when the bytes went out
        bool sendToTarget(mark4_NodeKind target,
                          const mark4_Envelope &envelope,
                          std::string &errorOut);

        /// @brief Applies one discovery event: logs it, pushes the configured
        ///        profile, and tells the clients what changed.
        /// @param change event to apply
        void onDiscoveryChange(const DiscoveryChange &change);

        /// @brief Makes one drone THE connected drone. Connecting elsewhere
        ///        replaces the previous connection.
        /// @param message decoded connect request
        /// @param errorOut receives the reason when it cannot be
        /// @return true when the connection is established
        bool applyConnect(const ClientMessage &message, std::string &errorOut);

        /// @brief Drops the connected drone. A no-op when nothing is
        ///        connected.
        void applyDisconnect();

        /// @brief Says whether a command aimed at one kind may go out: only
        ///        the connected drone is wired to the controls.
        /// @param target kind the command is aimed at
        /// @param errorOut receives the refusal reason
        /// @return true when the command may go out
        bool commandAllowed(mark4_NodeKind target, std::string &errorOut) const;

        /// @brief Recomputes whether the connected drone shows signs of life
        ///        and tells the clients when the answer changed. The
        ///        connection itself is never dropped here: losing the drone
        ///        and letting it go are two different things, and only the
        ///        operator does the second.
        void refreshConnection();

        /// @brief Decodes and routes everything the clients have sent.
        void handleClientMessages();

        /// @brief Carries out one decoded client request.
        /// @param message request to carry out
        /// @param errorOut receives the refusal reason when it cannot be
        /// @return true when the request was carried out
        bool applyClientMessage(const ClientMessage &message, std::string &errorOut);

        /// @brief Answers one request, when the client asked to be answered.
        ///        The answer is broadcast: it carries the correlation id the
        ///        client sent, and a client ignores what is not its own.
        /// @param id correlation id, -1 for a request that wants no answer
        /// @param ok true when the request was carried out
        /// @param error refusal reason, empty when ok
        void answer(int id, bool ok, const std::string &error);

        /// @brief Sends one whole profile to a process, parameter by
        ///        parameter. Nothing stores tuning on the flight side, so a
        ///        push is the only way values survive a restart.
        /// @param name profile to push
        /// @param target kind of process to push it to
        /// @param errorOut receives the reason when the push cannot happen
        /// @return true when every value went out
        bool pushProfile(const std::string &name, mark4_NodeKind target, std::string &errorOut);

        /// @brief Sends the current discovery table to the clients.
        void broadcastDiscovery();

        /// @brief Sends the current counters to the clients.
        void broadcastStatus();

        /// @brief Sends the state of the update client to the clients. Called
        ///        on every observable change of that client, which is what
        ///        makes a progress bar move without anybody polling.
        void broadcastOta();

        /// @brief Carries out one update request.
        /// @param message request to carry out
        /// @param errorOut receives the refusal reason when it cannot be
        /// @return true when the request was carried out
        bool applyOtaMessage(const ClientMessage &message, std::string &errorOut);

        /// @brief Expiry, status message and connection liveness.
        /// @param nowUs current time [us]
        void housekeeping(std::uint64_t nowUs);

        /// @return the counters and flags the status message carries
        [[nodiscard]] HubStatus status() const;

        /// The one drone the operator is connected to. Everything the hub
        /// hears is still decoded, recorded and published; being connected
        /// decides where the commands go and which drone the control page
        /// pilots. Losing the drone keeps the connection with live = false,
        /// and the same drone coming back turns live true again on its own.
        struct Connection
        {
            std::string via;                               ///< "udp", empty = none
            std::string id;                                ///< kind name
            mark4_NodeKind kind = mark4_NodeKind_FIRMWARE; ///< kind commands route to
            bool live = false;                             ///< the drone shows signs of life
        };

        Config m_config;                                      ///< settings of this run
        TuningProfiles m_profiles;                            ///< stored tuning profiles
        DiscoveryRegistry m_registry;                         ///< live processes
        UdpLink m_udpLink;                                    ///< the LAN link, the only one
        Transport m_transport;                                ///< this hub as a transport node
        WsBridge m_ws;                                        ///< websocket endpoint
        Connection m_connection;                              ///< the one drone commands go to
        std::atomic_bool m_stopRequested{false};              ///< set by a signal handler
        std::uint64_t m_nextStatusUs = 0U;                    ///< next status message instant [us]
        std::uint64_t m_badFrames = 0U;                       ///< payloads that decoded to nothing
        std::uint32_t m_scenarioSequence = 0U;                ///< rolling number stamped on a
                                                              ///< scenario the client left at 0
        std::map<std::string, std::uint64_t> m_rcSeenUs;      ///< last RC instant per client
        OtaClient m_ota;                                      ///< firmware update session
        mark4_NodeKind m_otaTarget = mark4_NodeKind_FIRMWARE; ///< process the updater talks to
    };
} // namespace mark4
