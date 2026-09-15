/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains the transport and the websocket, carries out the clients'
///        commands and publishes what the consumers hold.

#include "hub/hub_app.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <poll.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "hub/gateway_codec.hpp"
#include "log/module.hpp"
#include "log_modules.hpp"
#include "transport/node_id.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_GATEWAY_CORE, "gateway/core"};

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

        /// @param kind kind a node announced
        /// @return its name, for the log
        const char *kindName(mark4_NodeKind kind)
        {
            switch (kind)
            {
                case mark4_NodeKind_FIRMWARE:
                    return "firmware";
                case mark4_NodeKind_DRONE_SIM:
                    return "drone_sim";
                case mark4_NodeKind_PLANT:
                    return "plant";
                case mark4_NodeKind_GATEWAY:
                    return "gateway";
                case mark4_NodeKind_BATCH:
                    return "batch";
                case mark4_NodeKind_RELAY:
                    return "relay";
                case mark4_NodeKind_PHONE:
                    return "phone";
                case mark4_NodeKind_NODE_KIND_UNSPECIFIED:
                    break;
            }
            return "unknown";
        }

        /// @return "hub-<hostname>", cut to what an Announce name holds
        std::string gatewayName()
        {
            std::array<char, PATH_BUFFER_SIZE> host{};
            std::string name = "hub";
            if (::gethostname(host.data(), host.size() - 1U) == 0 && host[0] != '\0')
            {
                name += std::string("-") + host.data();
            }
            return name.substr(0U, sizeof(mark4_Announce::name) - 1U);
        }

        /// @return the gateway's own identity: kind GATEWAY, mcu SIM, this
        ///         build's wire hash, the host's name, and no build identity
        ///         (a process is not a packaged image)
        mark4_Announce gatewayAnnounce()
        {
            mark4_Announce announce = mark4_Announce_init_zero;
            announce.kind = mark4_NodeKind_GATEWAY;
            announce.mcu = mark4_Mcu_SIM;
            announce.wire_hash = WIRE_HASH;
            copyWireString(gatewayName(), announce.name, sizeof(announce.name));
            return announce;
        }
    } // namespace

    HubApp::HubApp(Config config)
        : m_config(std::move(config)),
          m_profiles(m_config.profilesDir),
          m_udpLink(m_config.discoveryPort),
          m_ownAnnounce(gatewayAnnounce()),
          m_transport(m_config.nodeId != 0U ? m_config.nodeId : randomNodeId(), randomBootId())
    {
    }

    std::uint64_t HubApp::LogClock(void *context)
    {
        static_cast<void>(context);
        return monotonicUs();
    }

    bool HubApp::init()
    {
        logSetClock(&HubApp::LogClock, this);
        static_cast<void>(logAddSink(m_consoleSink));
        // One link, the LAN: every drone is a node on it, the board through
        // the relay riding it (esp32-bridge/), so there is nothing to relay
        // here and no second path to open.
        if (!m_udpLink.init() || !m_transport.addLink(m_udpLink) || !m_transport.init())
        {
            MODULE.error("cannot start the transport");
            return false;
        }
        if (!m_messenger.init())
        {
            MODULE.error("a message tag is claimed twice");
            return false;
        }
        if (!m_directory.init())
        {
            MODULE.error("too many directory listeners");
            return false;
        }
        if (!m_status.init() || !m_logs.init() || !m_telemetryTables.init() || !m_tuning.init())
        {
            MODULE.error("too many consumer listeners");
            return false;
        }
        // A messenger refuses this node as a destination, so the gateway's
        // own lines reach its clients through the log gateway instead.
        m_logProvider.setLocalSink(&m_logGateway);
        static_cast<void>(logAddSink(m_logProvider));
        MODULE.info("boot: node %s on discovery udp/%u, wire %08x",
                    hexNodeId(m_transport.nodeId()).c_str(),
                    static_cast<unsigned>(m_udpLink.discoveryPort()),
                    WIRE_HASH);
        if (m_config.pagesDir.empty())
        {
            m_config.pagesDir = defaultProjectPath(DEFAULT_PAGES_DIR);
        }
        std::error_code failure;
        if (!std::filesystem::is_directory(m_config.pagesDir, failure))
        {
            // A hub without pages still serves the websocket: the pages are
            // a client, not a dependency.
            MODULE.warn("no pages in %s, serving none", m_config.pagesDir.c_str());
        }
        if (m_config.telemetryDir.empty())
        {
            // Nothing has been recorded yet on a fresh checkout, so the
            // directory need not exist: the first save creates it.
            m_config.telemetryDir = defaultProjectPath(DEFAULT_TELEMETRY_DIR, false);
        }
        HttpConfig http;
        http.pagesDir = m_config.pagesDir;
        http.telemetryDir = m_config.telemetryDir;
        MODULE.info("telemetry store in %s", m_config.telemetryDir.c_str());
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
        // The update client owns no socket: its messages are unicasts to the
        // node the OtaCommand named.
        m_ota.setSink([this](const mark4_Envelope &envelope, std::string &errorOut) {
            return sendEnvelope(m_otaTarget, envelope, errorOut);
        });
        m_ota.setOnChange([this]() { broadcast(otaMessage()); });
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

            m_messenger.poll(monotonicUs());
            m_directory.tick(monotonicUs());
            handleClientMessages();
            housekeeping(monotonicUs());
        }
        return 0;
    }

    void HubApp::PresenceListener::onNodeUp(const Transport::Node &node)
    {
        MODULE.info("node %s appeared", hexNodeId(node.id).c_str());
        m_app.m_nodesDirty = true;
    }

    void HubApp::PresenceListener::onNodeDown(const Transport::Node &node)
    {
        // What the node took with it is each consumer's business: they hear
        // the same event through the messenger.
        MODULE.info("node %s disappeared", hexNodeId(node.id).c_str());
        m_app.m_nodesDirty = true;
    }

    void HubApp::IdentityListener::onIdentity(const DirectoryEntry &entry)
    {
        // What a kind carries is each consumer's own constant: they listen
        // to this same directory and subscribe and pull for themselves.
        m_app.m_nodesDirty = true;
        MODULE.info("node %s is %s \"%s\"",
                    hexNodeId(entry.id).c_str(),
                    kindName(entry.announce.kind),
                    entry.announce.name);
        if (entry.wireMismatch)
        {
            MODULE.warn("node %s speaks wire %08x, this gateway speaks %08x: "
                        "rebuild and reflash it",
                        hexNodeId(entry.id).c_str(),
                        entry.announce.wire_hash,
                        WIRE_HASH);
        }
    }

    void HubApp::IdentityListener::onForgotten(std::uint32_t nodeId)
    {
        // The node table is republished from the presence event; the
        // directory entry is what went here.
        static_cast<void>(nodeId);
    }

    bool HubApp::OtaReader::onMessage(std::uint32_t src,
                                      const mark4_Envelope &envelope,
                                      std::uint64_t nowUs)
    {
        // The session keeps its own clock through tick(), and the gateway's
        // timers all read monotonicUs().
        static_cast<void>(src);
        static_cast<void>(nowUs);
        return m_app.m_ota.onEnvelope(envelope, monotonicUs());
    }

    bool HubApp::HubRequester::onMessage(std::uint32_t src,
                                         const mark4_Envelope &envelope,
                                         std::uint64_t nowUs)
    {
        static_cast<void>(src);
        static_cast<void>(envelope);
        static_cast<void>(nowUs);
        return false;
    }

    bool HubApp::HubRequester::ask(std::uint32_t dst, mark4_Envelope &envelope)
    {
        return request(dst, envelope) != 0U;
    }

    bool HubApp::sendEnvelope(std::uint32_t dst,
                              const mark4_Envelope &envelope,
                              std::string &errorOut)
    {
        if (!m_messenger.send(dst, envelope))
        {
            // The messenger refuses a broadcast destination, a message that
            // does not encode, and a node the transport cannot reach.
            errorOut = "node " + hexNodeId(dst) +
                       " is not reachable (or the destination is the broadcast, or the "
                       "message does not encode)";
            return false;
        }
        return true;
    }

    void HubApp::handleClientMessages()
    {
        for (const InboundMessage &inbound : m_ws.drainInbound())
        {
            mark4_GatewayMessage message;
            std::string error;
            if (!decodeGatewayMessage(reinterpret_cast<const std::uint8_t *>( // NOLINT
                                          inbound.bytes.data()),
                                      inbound.bytes.size(),
                                      message))
            {
                ++m_refusedCommands;
                continue;
            }
            const bool done = applyClientMessage(message, inbound.clientId, error);
            if (done)
            {
                ++m_commands;
            }
            else
            {
                ++m_refusedCommands;
            }
            if (message.id != 0U)
            {
                mark4_GatewayMessage ack = mark4_GatewayMessage_init_zero;
                ack.which_body = mark4_GatewayMessage_ack_tag;
                ack.id = message.id;
                ack.body.ack.ok = done;
                copyWireString(error, ack.body.ack.error, sizeof(ack.body.ack.error));
                broadcast(ack);
            }
        }
    }

    bool HubApp::applyClientMessage(const mark4_GatewayMessage &message,
                                    const std::string &clientId,
                                    std::string &errorOut)
    {
        switch (message.which_body)
        {
            case mark4_GatewayMessage_ota_command_tag:
                return applyOtaCommand(
                    m_ota, message.body.ota_command, m_otaTarget, monotonicUs(), errorOut);
            case mark4_GatewayMessage_profile_command_tag:
                return applyProfileCommand(message.body.profile_command, errorOut);
            case mark4_GatewayMessage_telemetry_command_tag:
                return m_telemetryGateway.apply(message.body.telemetry_command, clientId, errorOut);
            case mark4_GatewayMessage_log_command_tag:
                return m_logGateway.apply(message.body.log_command, clientId, errorOut);
            case mark4_GatewayMessage_tuning_command_tag:
                return m_tuningGateway.apply(message.body.tuning_command, clientId, errorOut);
            case mark4_GatewayMessage_pilot_input_tag:
                return m_pilotGateway.apply(
                    message.body.pilot_input, clientId, monotonicUs(), errorOut);
            case mark4_GatewayMessage_node_command_tag:
                return applyNodeCommand(message.body.node_command, errorOut);
            default:
                errorOut = "unsupported message";
                return false;
        }
    }

    bool HubApp::applyProfileCommand(const mark4_ProfileCommand &command, std::string &errorOut)
    {
        mark4_GatewayMessage answer = mark4_GatewayMessage_init_zero;
        switch (command.op)
        {
            case mark4_ProfileCommand_Op_SAVE:
                if (!m_profiles.save(command.name,
                                     tuningValuesOf(command.values, command.values_count),
                                     errorOut))
                {
                    return false;
                }
                [[fallthrough]];
            case mark4_ProfileCommand_Op_LIST: {
                answer.which_body = mark4_GatewayMessage_profiles_tag;
                for (const std::string &name : m_profiles.list())
                {
                    mark4_ProfileList &list = answer.body.profiles;
                    if (list.names_count >= std::size(list.names))
                    {
                        break;
                    }
                    copyWireString(name, list.names[list.names_count], sizeof(list.names[0]));
                    ++list.names_count;
                }
                broadcast(answer);
                return true;
            }
            case mark4_ProfileCommand_Op_LOAD: {
                TuningValues values;
                if (!m_profiles.load(command.name, values, errorOut))
                {
                    return false;
                }
                answer.which_body = mark4_GatewayMessage_profile_tag;
                fillProfile(command.name, values, answer.body.profile);
                broadcast(answer);
                return true;
            }
            case mark4_ProfileCommand_Op_PUSH: {
                const std::uint32_t target = command.target_node;
                return pushProfile(
                    m_profiles,
                    command.name,
                    target,
                    [this, target](std::uint32_t id, float value) {
                        return m_tuningGateway.set(target, id, value);
                    },
                    errorOut);
            }
            default:
                errorOut = "unsupported profile command";
                return false;
        }
    }

    bool HubApp::applyNodeCommand(const mark4_NodeCommand &command, std::string &errorOut)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        switch (command.which_action)
        {
            case mark4_NodeCommand_reboot_tag:
                envelope.which_body = mark4_Envelope_reboot_tag;
                break;
            case mark4_NodeCommand_scenario_tag:
                envelope.which_body = mark4_Envelope_sim_scenario_tag;
                envelope.body.sim_scenario = command.action.scenario;
                break;
            default:
                errorOut = "empty node command";
                return false;
        }
        if (!m_requester.ask(command.node, envelope))
        {
            errorOut = "node " + hexNodeId(command.node) + " is not reachable";
            return false;
        }
        return true;
    }

    void HubApp::broadcast(const mark4_GatewayMessage &message)
    {
        std::string bytes;
        if (encodeGatewayMessage(message, bytes))
        {
            m_ws.broadcastBinary(bytes);
        }
    }

    void HubApp::sendTo(const std::string &clientId, const mark4_GatewayMessage &message)
    {
        std::string bytes;
        if (encodeGatewayMessage(message, bytes))
        {
            static_cast<void>(m_ws.sendBinary(clientId, bytes));
        }
    }

    mark4_GatewayMessage HubApp::nodesMessage()
    {
        const std::uint64_t nowUs = monotonicUs();
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_nodes_tag;
        mark4_NodeTable &table = message.body.nodes;
        // The gateway itself first: the transport's table never holds it.
        Transport::Node self;
        self.id = m_transport.nodeId();
        self.lastSeenUs = nowUs;
        fillNode(self, nowUs, &m_ownAnnounce, table.nodes[0]);
        table.nodes_count = 1U;
        for (std::size_t i = 0U; i < m_transport.nodeCount(); ++i)
        {
            const Transport::Node &node = m_transport.node(i);
            // PENDING and MUTE show as a node without identity: asked, no
            // answer yet or ever.
            const DirectoryEntry *entry = m_directory.find(node.id);
            const bool known = entry != nullptr && entry->state == DirectoryEntry::State::KNOWN;
            fillNode(
                node, nowUs, known ? &entry->announce : nullptr, table.nodes[table.nodes_count]);
            ++table.nodes_count;
        }
        return message;
    }

    mark4_GatewayMessage HubApp::statusMessage()
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_status_tag;
        mark4_GatewayStatus &status = message.body.status;
        status.node_id = m_transport.nodeId();
        status.wire_hash = WIRE_HASH;
        status.clients = static_cast<std::uint32_t>(m_ws.clientCount());
        status.rc_clients = static_cast<std::uint32_t>(m_pilotGateway.seats());
        status.messages_in = m_messenger.received();
        status.commands = m_commands;
        status.dropped = m_transport.dropped();
        status.refused = m_refusedCommands;
        return message;
    }

    mark4_GatewayMessage HubApp::otaMessage()
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_ota_state_tag;
        message.body.ota_state = otaStateOf(m_ota, m_otaTarget);
        return message;
    }

    void HubApp::snapshot(const std::string &clientId)
    {
        // A client that just connected knows nothing yet: everything the
        // gateway holds goes to it, and to it alone.
        sendTo(clientId, statusMessage());
        sendTo(clientId, otaMessage());
        m_statusGateway.onClientConnected(clientId);
        m_logGateway.onClientConnected(clientId);
        m_telemetryGateway.onClientConnected(clientId);
        m_tuningGateway.onClientConnected(clientId);
        sendTo(clientId, nodesMessage());
    }

    void HubApp::housekeeping(std::uint64_t nowUs)
    {
        m_pilotGateway.tick(nowUs);
        m_ota.tick(nowUs);
        if (m_udpLink.loopbackFallback() && !m_loopbackWarned)
        {
            m_loopbackWarned = true;
            MODULE.warn("no route for 255.255.255.255: broadcasting on the loopback");
        }

        // What went with a client is what was kept per client: its marks on
        // the sample streams and the nodes it piloted.
        for (const std::string &clientId : m_ws.drainClosed())
        {
            m_telemetryGateway.onClientClosed(clientId);
            m_pilotGateway.onClientClosed(clientId);
        }
        for (const std::string &clientId : m_ws.drainConnected())
        {
            snapshot(clientId);
        }
        if (nowUs >= m_nextStatusUs)
        {
            m_nextStatusUs = nowUs + STATUS_PERIOD_MS * US_PER_MS;
            m_nodesDirty = true;
            broadcast(statusMessage());
        }
        if (m_nodesDirty)
        {
            m_nodesDirty = false;
            broadcast(nodesMessage());
        }
    }
} // namespace mark4
