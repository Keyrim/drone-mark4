/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains every source, routes every message and keeps the discovery
///        table honest.

#include "hub/hub_app.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>

#include "transport/node_id.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;

        /// Size of the buffer the path of the running executable is read into.
        constexpr std::size_t PATH_BUFFER_SIZE = 4096U;

        /// @brief Default path of a directory or a file of the source tree,
        ///        resolved from the running executable so a build tree
        ///        anywhere works, and falling back to the path as written,
        ///        relative to the current directory.
        /// @param relative path relative to the source tree root
        /// @param mustExist true to only accept a resolved path that exists,
        ///        which is what a directory of assets wants; false for a
        ///        build artifact that is legitimately absent until it is built
        /// @return the path
        std::string defaultProjectPath(const char *relative, bool mustExist = true)
        {
            std::array<char, PATH_BUFFER_SIZE> path{};
            const ssize_t size = ::readlink("/proc/self/exe", path.data(), path.size() - 1U);
            if (size > 0)
            {
                const std::string directory =
                    std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(size)))
                        .parent_path()
                        .string();
                // software/build/<preset>/hub is four levels below the root.
                const std::string candidate = directory + "/../../../../" + relative;
                std::error_code failure;
                if (!mustExist || std::filesystem::exists(candidate, failure))
                {
                    return candidate;
                }
            }
            return relative;
        }

        /// @return a monotonic timestamp [us]
        std::uint64_t monotonicUs()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        }

        /// @brief Encodes one envelope for a link.
        /// @param envelope message to encode
        /// @param[out] bytes destination
        /// @param[out] sizeOut bytes written
        /// @return true when the message encoded
        bool encodeForLink(const mark4_Envelope &envelope,
                           std::array<std::uint8_t, MAX_ENVELOPE_SIZE> &bytes,
                           std::size_t &sizeOut)
        {
            return encodeEnvelope(envelope, bytes.data(), bytes.size(), sizeOut);
        }
    } // namespace

    HubApp::HubApp(Config config)
        : m_config(std::move(config)),
          m_profiles(m_config.profilesDir),
          m_udpLink(m_config.discoveryPort),
          m_transport(m_config.nodeId != 0U ? m_config.nodeId : randomNodeId())
    {
    }

    bool HubApp::init()
    {
        // One link, the LAN: every drone is a node on it, the board through
        // the relay riding it (esp32-bridge/), so there is nothing to relay
        // here and no second path to open.
        if (!m_udpLink.init() || !m_transport.addLink(m_udpLink) || !m_transport.init())
        {
            static_cast<void>(std::fprintf(stderr, "hub: cannot start the transport\n"));
            return false;
        }
        // The hub's own beacon: every node learns the gateway and the schema
        // it speaks, and the flight processes learn where to unicast.
        mark4_Envelope announce = mark4_Envelope_init_zero;
        announce.which_body = mark4_Envelope_announce_tag;
        announce.body.announce.kind = mark4_NodeKind_GATEWAY;
        static_cast<void>(
            std::snprintf(announce.body.announce.name, sizeof(announce.body.announce.name), "hub"));
        announce.body.announce.wire_hash = WIRE_HASH;
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> beacon{};
        std::size_t beaconSize = 0U;
        if (!encodeForLink(announce, beacon, beaconSize) || beaconSize > Transport::MAX_BEACON_SIZE)
        {
            static_cast<void>(std::fprintf(stderr, "hub: the announce does not fit a beacon\n"));
            return false;
        }
        m_transport.setBeacon(beacon.data(), beaconSize);
        if (m_config.pagesDir.empty())
        {
            m_config.pagesDir = defaultProjectPath(DEFAULT_PAGES_DIR);
        }
        std::error_code failure;
        if (!std::filesystem::is_directory(m_config.pagesDir, failure))
        {
            // A hub without pages still decodes and serves the websocket:
            // the pages are a client, not a dependency.
            static_cast<void>(
                std::printf("hub: no pages in %s, serving none\n", m_config.pagesDir.c_str()));
        }
        HttpConfig http;
        http.pagesDir = m_config.pagesDir;
        if (!m_ws.start(m_config.wsPort, m_config.bindAddress, std::move(http)))
        {
            return false;
        }

        if (m_config.otaBundlePath.empty())
        {
            // The bundle is a build artifact: resolving its path must not
            // depend on the build having happened yet. It is also shown to an
            // operator in the update panel, so the ".." the resolution walks
            // through is folded away rather than displayed.
            const std::filesystem::path candidate = defaultProjectPath(DEFAULT_OTA_BUNDLE, false);
            std::error_code unresolved;
            const std::filesystem::path folded =
                std::filesystem::weakly_canonical(candidate, unresolved);
            m_config.otaBundlePath = unresolved ? candidate.string() : folded.string();
        }
        m_ota.setDefaultBundlePath(m_config.otaBundlePath);
        // The update client owns no socket: it sends through the same routing
        // as every other command, so an updater message reaches the board
        // exactly like an RC frame does, a transport unicast to its node.
        m_ota.setSink([this](const mark4_Envelope &envelope, std::string &errorOut) {
            return sendToTarget(m_otaTarget, envelope, errorOut);
        });
        m_ota.setOnChange([this]() { broadcastOta(); });
        return true;
    }

    int HubApp::run(const std::function<bool()> &keepRunning)
    {
        std::vector<pollfd> fds;
        m_nextStatusUs = monotonicUs();
        while (!m_stopRequested.load() && (!keepRunning || keepRunning()))
        {
            fds.clear();
            for (const int fd : {m_udpLink.discoveryFd(), m_udpLink.dataFd()})
            {
                pollfd entry{};
                entry.fd = fd;
                entry.events = POLLIN;
                fds.push_back(entry);
            }
            // A transfer is paced from this loop, so the loop has to come
            // round on the pacing scale rather than on the idle one.
            static_cast<void>(::poll(
                fds.data(), fds.size(), m_ota.busy() ? OTA_POLL_TIMEOUT_MS : POLL_TIMEOUT_MS));

            m_transport.poll(monotonicUs(), &HubApp::OnFrame, this);
            handleClientMessages();
            housekeeping(monotonicUs());
        }
        return 0;
    }

    void HubApp::OnFrame(void *context,
                         std::uint32_t src,
                         const std::uint8_t *data,
                         std::size_t size)
    {
        static_cast<HubApp *>(context)->onFrame(src, data, size);
    }

    void HubApp::onFrame(std::uint32_t src, const std::uint8_t *data, std::size_t size)
    {
        mark4_Envelope envelope;
        if (!decodeEnvelope(data, size, envelope))
        {
            ++m_badFrames;
            return;
        }
        // The kind comes from the node's own announce; a node that never
        // announced (or announced on another schema) still has its telemetry
        // rendered, under the kind nobody claims.
        mark4_NodeKind source = mark4_NodeKind_NODE_KIND_UNSPECIFIED;
        static_cast<void>(m_registry.kindOf(src, source));
        onEnvelope(envelope, src, source);
    }

    void HubApp::onEnvelope(const mark4_Envelope &envelope,
                            std::uint32_t nodeId,
                            mark4_NodeKind kind)
    {
        const std::uint64_t nowUs = monotonicUs();
        switch (envelope.which_body)
        {
            case mark4_Envelope_announce_tag: {
                // The beacon of a node: the node it came from is the address
                // every command to that process goes to.
                const auto change = m_registry.onAnnounce(nodeId, envelope.body.announce, nowUs);
                if (change.has_value())
                {
                    onDiscoveryChange(*change);
                }
                return;
            }
            case mark4_Envelope_telemetry_tag:
                m_ws.broadcastText(telemetryToJson(envelope.body.telemetry, kind));
                if (envelope.body.telemetry.has_truth)
                {
                    // The plant's exact state rides inside the estimate it is
                    // compared against; the pages read it as its own message.
                    m_ws.broadcastText(simRawToJson(envelope.body.telemetry, kind));
                }
                return;
            case mark4_Envelope_tuning_ack_tag:
                m_ws.broadcastText(tuningAckToJson(envelope.body.tuning_ack, kind));
                return;
            case mark4_Envelope_tuning_info_tag:
                m_ws.broadcastText(tuningInfoToJson(envelope.body.tuning_info, kind));
                return;
            case mark4_Envelope_log_tag:
                // A console line of a node without a console: the board
                // behind its relay, updated over the air with no probe on.
                static_cast<void>(
                    std::printf("%s: %s\n", nodeKindName(kind), envelope.body.log.text));
                static_cast<void>(std::fflush(stdout));
                m_ws.broadcastText(logToJson(envelope.body.log, kind));
                return;
            default:
                // Updater answers are one more body on whatever link the
                // process being updated is reachable on. Past the updater,
                // a body this hub does not render (run stats, logs, another
                // node's commands) is simply not for it.
                static_cast<void>(m_ota.onEnvelope(envelope, nowUs));
                return;
        }
    }

    bool HubApp::sendToTarget(mark4_NodeKind target,
                              const mark4_Envelope &envelope,
                              std::string &errorOut)
    {
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeForLink(envelope, bytes, size))
        {
            errorOut = "the command does not encode";
            return false;
        }
        const std::uint32_t nodeId = m_registry.nodeIdOf(target);
        if (nodeId == 0U)
        {
            errorOut = std::string("no process of kind ") + nodeKindName(target);
            return false;
        }
        if (!m_transport.send(nodeId, bytes.data(), size))
        {
            errorOut =
                std::string("transport node of ") + nodeKindName(target) + " is not reachable";
            return false;
        }
        return true;
    }

    void HubApp::onDiscoveryChange(const DiscoveryChange &change)
    {
        const char *kind = nodeKindName(change.process.kind);
        switch (change.event)
        {
            case DiscoveryEvent::APPEARED:
                static_cast<void>(
                    std::printf("hub: %s appeared (node %u)\n", kind, change.process.nodeId));
                break;
            case DiscoveryEvent::RESTARTED:
                static_cast<void>(
                    std::printf("hub: %s restarted (node %u)\n", kind, change.process.nodeId));
                break;
            case DiscoveryEvent::DISAPPEARED:
                static_cast<void>(std::printf("hub: %s disappeared\n", kind));
                break;
        }
        if (change.event != DiscoveryEvent::DISAPPEARED && change.process.wireMismatch)
        {
            // The one silent failure of a wire change: a node built on another
            // schema keeps sending well formed envelopes this hub decodes into
            // nonsense, or not at all. Named here, once per appearance.
            static_cast<void>(std::fprintf(
                stderr,
                "hub: %s speaks wire %08x, this hub speaks %08x: rebuild and reflash it, "
                "it cannot be trusted or updated from here (docs/ota-design.md)\n",
                kind,
                change.process.wireHash,
                WIRE_HASH));
        }
        if (!m_config.pushProfileName.empty() && change.event != DiscoveryEvent::DISAPPEARED)
        {
            // A flight process has no flash: it boots on the defaults every
            // time. Pushing on the announce is what makes a bench session
            // survive a restart of the thing being tuned.
            std::string error;
            if (pushProfile(m_config.pushProfileName, change.process.kind, error))
            {
                static_cast<void>(std::printf(
                    "hub: pushed profile %s to %s\n", m_config.pushProfileName.c_str(), kind));
            }
            else
            {
                static_cast<void>(std::fprintf(stderr,
                                               "hub: cannot push profile %s to %s: %s\n",
                                               m_config.pushProfileName.c_str(),
                                               kind,
                                               error.c_str()));
            }
        }
        static_cast<void>(std::fflush(stdout));
        broadcastDiscovery();
    }

    bool HubApp::pushProfile(const std::string &name, mark4_NodeKind target, std::string &errorOut)
    {
        TuningValues values;
        if (!m_profiles.load(name, values, errorOut))
        {
            return false;
        }
        for (const auto &[id, value] : values)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_tuning_set_tag;
            envelope.body.tuning_set.id = id;
            envelope.body.tuning_set.value = value;
            if (!sendToTarget(target, envelope, errorOut))
            {
                return false;
            }
        }
        return true;
    }

    void HubApp::handleClientMessages()
    {
        for (const InboundText &inbound : m_ws.drainInbound())
        {
            const auto decoded = parseClientMessage(inbound.text);
            if (const auto *reason = std::get_if<std::string>(&decoded))
            {
                answer(clientMessageId(inbound.text), false, *reason);
                continue;
            }
            const auto &message = std::get<ClientMessage>(decoded);
            if (message.type == ClientMessageType::RC)
            {
                // Remember who pilots, so the status can warn about a second
                // pilot without counting the tabs that only watch.
                m_rcSeenUs[inbound.clientId] = monotonicUs();
            }
            std::string error;
            const bool done = applyClientMessage(message, error);
            answer(message.id, done, error);
        }
    }

    bool HubApp::commandAllowed(mark4_NodeKind target, std::string &errorOut) const
    {
        if (m_connection.via.empty())
        {
            errorOut = "no drone connected";
            return false;
        }
        if (target != m_connection.kind)
        {
            errorOut =
                std::string("connected to ") + m_connection.id + ", not to " + nodeKindName(target);
            return false;
        }
        return true;
    }

    bool HubApp::applyClientMessage(const ClientMessage &message, std::string &errorOut)
    {
        // Only the connected drone is wired to the controls: a drone that
        // merely announces itself is visible, not commandable. The profiles
        // on disk are hub business and need no drone at all.
        switch (message.type)
        {
            case ClientMessageType::RC:
            case ClientMessageType::SIM_SCENARIO:
            case ClientMessageType::REBOOT:
            case ClientMessageType::TUNING_SET:
            case ClientMessageType::TUNING_LIST:
            case ClientMessageType::PROFILE_PUSH:
            case ClientMessageType::OTA_STATUS:
            case ClientMessageType::OTA_START:
            case ClientMessageType::OTA_REVERT:
                if (!commandAllowed(message.target, errorOut))
                {
                    return false;
                }
                break;
            default:
                // OTA_ABORT stays reachable: dropping a stuck transfer is hub
                // business and must work even after the drone is gone.
                break;
        }
        switch (message.type)
        {
            case ClientMessageType::RC:
                // The hub forwards RC one for one and never repeats or
                // synthesizes it: a silent link means kill downstream, and
                // that silence is the pilot's, not the hub's to fill in.
            case ClientMessageType::TUNING_SET:
            case ClientMessageType::TUNING_LIST:
                // The ack the client gets back says the request went out, not
                // that the table arrived: the descriptions come as their own
                // messages, one per flight frame, as the process unrolls them.
                return sendToTarget(message.target, message.command, errorOut);
            case ClientMessageType::SIM_SCENARIO: {
                mark4_Envelope envelope = message.command;
                if (envelope.body.sim_scenario.sequence == 0U)
                {
                    // 0 means "no scenario" on the wire, so a client that
                    // sent none gets the hub's own rolling number: two
                    // scenarios in a row are then two scenarios, not one.
                    m_scenarioSequence = m_scenarioSequence % MAX_SCENARIO_SEQUENCE + 1U;
                    envelope.body.sim_scenario.sequence = m_scenarioSequence;
                }
                // Routed like every other command, to the node that
                // announced; the flight process forwards it to its plant.
                return sendToTarget(message.target, envelope, errorOut);
            }
            case ClientMessageType::REBOOT: {
                if (message.target != mark4_NodeKind_FIRMWARE)
                {
                    errorOut = std::string("cannot reboot ") + nodeKindName(message.target);
                    return false;
                }
                return sendToTarget(message.target, message.command, errorOut);
            }
            case ClientMessageType::PROFILE_LIST: {
                m_ws.broadcastText(profileNamesToJson(m_profiles.list()));
                return true;
            }
            case ClientMessageType::PROFILE_SAVE: {
                if (!m_profiles.save(message.profileName, message.profileValues, errorOut))
                {
                    return false;
                }
                m_ws.broadcastText(profileNamesToJson(m_profiles.list()));
                return true;
            }
            case ClientMessageType::PROFILE_LOAD: {
                TuningValues values;
                if (!m_profiles.load(message.profileName, values, errorOut))
                {
                    return false;
                }
                m_ws.broadcastText(profileToJson(message.profileName, values));
                return true;
            }
            case ClientMessageType::PROFILE_PUSH: {
                return pushProfile(message.profileName, message.target, errorOut);
            }
            case ClientMessageType::CONNECT: {
                return applyConnect(message, errorOut);
            }
            case ClientMessageType::DISCONNECT: {
                applyDisconnect();
                return true;
            }
            case ClientMessageType::OTA_STATUS:
            case ClientMessageType::OTA_START:
            case ClientMessageType::OTA_ABORT:
            case ClientMessageType::OTA_REVERT: {
                return applyOtaMessage(message, errorOut);
            }
        }
        errorOut = "unsupported request";
        return false;
    }

    bool HubApp::applyOtaMessage(const ClientMessage &message, std::string &errorOut)
    {
        const std::uint64_t nowUs = monotonicUs();
        // The route is fixed for the whole session: an update that started on
        // the board must not have half of it delivered to a simulator because
        // a later message named another target.
        if (!m_ota.busy())
        {
            m_otaTarget = message.target;
        }
        switch (message.type)
        {
            case ClientMessageType::OTA_STATUS:
                return m_ota.requestBoardStatus(nowUs, errorOut);
            case ClientMessageType::OTA_START:
                return m_ota.start(message.otaBundlePath, nowUs, errorOut);
            case ClientMessageType::OTA_ABORT:
                return m_ota.abortSession(nowUs, errorOut);
            case ClientMessageType::OTA_REVERT:
                return m_ota.revert(nowUs, errorOut);
            default:
                errorOut = "unsupported update request";
                return false;
        }
    }

    bool HubApp::applyConnect(const ClientMessage &message, std::string &errorOut)
    {
        // Every drone is a node of the LAN, the board included (its relay
        // carries its frames): the identity of a connection is the kind.
        static_cast<void>(errorOut);
        Connection next;
        next.via = message.connectVia;
        next.id = nodeKindName(message.target);
        next.kind = message.target;
        m_connection = next;
        refreshConnection();
        broadcastStatus();
        return true;
    }

    void HubApp::applyDisconnect()
    {
        m_connection = Connection{};
        broadcastStatus();
    }

    void HubApp::refreshConnection()
    {
        if (m_connection.via.empty())
        {
            return;
        }
        // Alive = its Announce is still fresh in the registry; the registry
        // expires it on the transport's own delay.
        const auto &processes = m_registry.processes();
        const bool live = std::any_of(
            processes.begin(), processes.end(), [this](const DiscoveredProcess &process) {
                return process.kind == m_connection.kind;
            });
        if (live != m_connection.live)
        {
            m_connection.live = live;
            broadcastStatus();
        }
    }

    void HubApp::answer(int id, bool ok, const std::string &error)
    {
        // Streams are never acknowledged, and neither is a request that
        // carried no correlation id: there would be nothing to match it to.
        if (id < 0)
        {
            return;
        }
        m_ws.broadcastText(ackToJson(id, ok, error));
    }

    void HubApp::broadcastDiscovery()
    {
        m_ws.broadcastText(discoveryToJson(m_registry.processes(), monotonicUs()));
    }

    void HubApp::broadcastStatus()
    {
        m_ws.broadcastText(statusToJson(status()));
    }

    void HubApp::broadcastOta()
    {
        m_ws.broadcastText(otaToJson(m_ota));
    }

    void HubApp::housekeeping(std::uint64_t nowUs)
    {
        for (const DiscoveryChange &change : m_registry.expire(nowUs, DISCOVERY_EXPIRY_US))
        {
            onDiscoveryChange(change);
        }
        refreshConnection();
        std::erase_if(m_rcSeenUs, [nowUs](const auto &entry) {
            return nowUs - entry.second > RC_PILOT_WINDOW_US;
        });
        m_ota.tick(nowUs);

        // A client that just connected knows nothing yet: it gets the table
        // and the counters as they stand, without waiting for a change.
        if (m_ws.takeConnectedFlag())
        {
            broadcastDiscovery();
            broadcastStatus();
            broadcastOta();
        }

        if (nowUs >= m_nextStatusUs)
        {
            m_nextStatusUs = nowUs + STATUS_PERIOD_MS * US_PER_MS;
            broadcastStatus();
        }
    }

    HubStatus HubApp::status() const
    {
        HubStatus snapshot;
        snapshot.connectionVia = m_connection.via;
        snapshot.connectionId = m_connection.id;
        snapshot.connectionKind = m_connection.kind;
        snapshot.connectionLive = m_connection.live;
        snapshot.badFrames = m_badFrames;
        snapshot.rejectedAnnounces = m_registry.rejectedAnnounces();
        snapshot.clients = m_ws.clientCount();
        const std::uint64_t nowUs = monotonicUs();
        snapshot.rcClients = static_cast<std::size_t>(
            std::count_if(m_rcSeenUs.begin(), m_rcSeenUs.end(), [nowUs](const auto &entry) {
                return nowUs - entry.second <= RC_PILOT_WINDOW_US;
            }));
        // One entry per process reached over the transport: the frame
        // counters of its node, every payload type included.
        for (const DiscoveredProcess &process : m_registry.processes())
        {
            const Transport::Node *node = m_transport.findNode(process.nodeId);
            if (node == nullptr)
            {
                continue;
            }
            LinkHealth link;
            link.sourceId = static_cast<std::uint8_t>(process.kind);
            link.sourceName = nodeKindName(process.kind);
            link.received = node->received;
            link.lost = node->lost;
            link.duplicates = node->duplicates;
            link.lastSequence = node->lastSeq;
            snapshot.links.push_back(link);
        }
        return snapshot;
    }
} // namespace mark4
