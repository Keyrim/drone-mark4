/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains the transport and the websocket, forwards frames both ways
///        and publishes the node table.

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
#include <vector>

#include "hub/gateway_codec.hpp"
#include "transport/node_id.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;

        /// Size of the buffer the path of the running executable is read into.
        constexpr std::size_t PATH_BUFFER_SIZE = 4096U;

        /// First byte of an encoded Envelope carrying an Rc: the one thing the
        /// gateway reads in a client frame, to count the pilots.
        constexpr std::uint8_t RC_TAG_BYTE =
            static_cast<std::uint8_t>((mark4_Envelope_rc_tag << PB_TAG_FIELD_SHIFT) | PB_WT_STRING);

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

        /// @return true when two beacons say the same thing
        bool sameAnnounce(const mark4_Announce &a, const mark4_Announce &b)
        {
            return a.kind == b.kind && a.mcu == b.mcu && a.build_epoch == b.build_epoch &&
                   a.wire_hash == b.wire_hash && std::strcmp(a.name, b.name) == 0 &&
                   std::strcmp(a.git_hash, b.git_hash) == 0;
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
        m_transport.setNodeCallbacks(&HubApp::OnNodeUp, &HubApp::OnNodeDown, this);
        // The gateway's own beacon: every node learns the gateway and the
        // schema it speaks, and the flight processes learn where to unicast.
        m_ownAnnounce.kind = mark4_NodeKind_GATEWAY;
        m_ownAnnounce.mcu = mark4_Mcu_SIM;
        m_ownAnnounce.wire_hash = WIRE_HASH;
        copyWireString(gatewayName(), m_ownAnnounce.name, sizeof(m_ownAnnounce.name));
        mark4_Envelope announce = mark4_Envelope_init_zero;
        announce.which_body = mark4_Envelope_announce_tag;
        announce.body.announce = m_ownAnnounce;
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> beacon{};
        std::size_t beaconSize = 0U;
        if (!encodeEnvelope(announce, beacon.data(), beacon.size(), beaconSize) ||
            beaconSize > Transport::MAX_BEACON_SIZE)
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
            // A hub without pages still forwards and serves the websocket:
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
        // The update client owns no socket: its messages are unicasts to the
        // node the OtaCommand named, exactly like a client's own frames.
        m_ota.setSink([this](const mark4_Envelope &envelope, std::string &errorOut) {
            return sendEnvelope(m_otaTarget, envelope, errorOut);
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

    void HubApp::OnNodeUp(void *context, const Transport::Node &node)
    {
        static_cast<void>(std::printf("hub: node %u appeared\n", node.id));
        static_cast<HubApp *>(context)->m_nodesDirty = true;
    }

    void HubApp::OnNodeDown(void *context, const Transport::Node &node)
    {
        static_cast<void>(std::printf("hub: node %u disappeared\n", node.id));
        auto *self = static_cast<HubApp *>(context);
        self->m_announces.erase(node.id);
        self->m_nodesDirty = true;
    }

    void HubApp::onFrame(std::uint32_t src, const std::uint8_t *data, std::size_t size)
    {
        ++m_framesIn;
        if (size > sizeof(mark4_Frame::payload.bytes))
        {
            return;
        }
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_frame_tag;
        message.body.frame.src = src;
        message.body.frame.payload.size = static_cast<pb_size_t>(size);
        std::memcpy(message.body.frame.payload.bytes, data, size);
        broadcast(message);

        // The two things the gateway reads: who is who, and the updater's
        // answers. Everything else is the clients' business.
        mark4_Envelope envelope;
        if (!decodeEnvelope(data, size, envelope))
        {
            return;
        }
        switch (envelope.which_body)
        {
            case mark4_Envelope_announce_tag: {
                auto [entry, inserted] = m_announces.try_emplace(src, envelope.body.announce);
                if (inserted || !sameAnnounce(entry->second, envelope.body.announce))
                {
                    entry->second = envelope.body.announce;
                    m_nodesDirty = true;
                    if (envelope.body.announce.wire_hash != WIRE_HASH)
                    {
                        static_cast<void>(std::fprintf(
                            stderr,
                            "hub: node %u speaks wire %08x, this hub speaks %08x: rebuild and "
                            "reflash it (docs/ota-design.md)\n",
                            src,
                            envelope.body.announce.wire_hash,
                            WIRE_HASH));
                    }
                }
                return;
            }
            case mark4_Envelope_log_tag:
                // A console line of a node without a console: printed here
                // too, for a bench with no page open.
                static_cast<void>(std::printf("node %u: %s\n", src, envelope.body.log.text));
                static_cast<void>(std::fflush(stdout));
                return;
            default:
                static_cast<void>(m_ota.onEnvelope(envelope, monotonicUs()));
                return;
        }
    }

    bool HubApp::sendEnvelope(std::uint32_t dst,
                              const mark4_Envelope &envelope,
                              std::string &errorOut)
    {
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeEnvelope(envelope, bytes.data(), bytes.size(), size))
        {
            errorOut = "the message does not encode";
            return false;
        }
        if (!m_transport.send(dst, bytes.data(), size))
        {
            errorOut = "node " + std::to_string(dst) + " is not reachable";
            return false;
        }
        ++m_framesOut;
        return true;
    }

    void HubApp::handleClientMessages()
    {
        for (const InboundMessage &inbound : m_ws.drainInbound())
        {
            mark4_GatewayMessage message;
            std::string error;
            bool done = false;
            if (!decodeGatewayMessage(reinterpret_cast<const std::uint8_t *>( // NOLINT
                                          inbound.bytes.data()),
                                      inbound.bytes.size(),
                                      message))
            {
                ++m_badFrames;
                continue;
            }
            done = applyClientMessage(message, inbound.clientId, error);
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
            case mark4_GatewayMessage_frame_tag: {
                const mark4_Frame &frame = message.body.frame;
                if (frame.payload.size == 0U)
                {
                    ++m_badFrames;
                    errorOut = "empty frame";
                    return false;
                }
                if (frame.payload.bytes[0] == RC_TAG_BYTE)
                {
                    // Remember who pilots, so the status can warn about a
                    // second pilot without counting the tabs that only watch.
                    m_rcSeenUs[clientId] = monotonicUs();
                }
                if (!m_transport.send(frame.dst, frame.payload.bytes, frame.payload.size))
                {
                    ++m_badFrames;
                    errorOut = "node " + std::to_string(frame.dst) + " is not reachable";
                    return false;
                }
                ++m_framesOut;
                return true;
            }
            case mark4_GatewayMessage_ota_command_tag:
                return applyOtaCommand(
                    m_ota, message.body.ota_command, m_otaTarget, monotonicUs(), errorOut);
            case mark4_GatewayMessage_profile_command_tag:
                return applyProfileCommand(message.body.profile_command, errorOut);
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
            case mark4_ProfileCommand_Op_PUSH:
                return pushProfile(
                    m_profiles,
                    command.name,
                    command.target_node,
                    [this](std::uint32_t dst, const mark4_Envelope &envelope, std::string &error) {
                        return sendEnvelope(dst, envelope, error);
                    },
                    errorOut);
            default:
                errorOut = "unsupported profile command";
                return false;
        }
    }

    void HubApp::broadcast(const mark4_GatewayMessage &message)
    {
        std::string bytes;
        if (encodeGatewayMessage(message, bytes))
        {
            m_ws.broadcastBinary(bytes);
        }
    }

    void HubApp::broadcastNodes()
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
            const auto announce = m_announces.find(node.id);
            fillNode(node,
                     nowUs,
                     announce == m_announces.end() ? nullptr : &announce->second,
                     table.nodes[table.nodes_count]);
            ++table.nodes_count;
        }
        broadcast(message);
        m_nodesDirty = false;
    }

    void HubApp::broadcastStatus()
    {
        const std::uint64_t nowUs = monotonicUs();
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_status_tag;
        mark4_GatewayStatus &status = message.body.status;
        status.node_id = m_transport.nodeId();
        status.wire_hash = WIRE_HASH;
        status.clients = static_cast<std::uint32_t>(m_ws.clientCount());
        status.rc_clients = static_cast<std::uint32_t>(
            std::count_if(m_rcSeenUs.begin(), m_rcSeenUs.end(), [nowUs](const auto &entry) {
                return nowUs - entry.second <= RC_PILOT_WINDOW_US;
            }));
        status.frames_in = m_framesIn;
        status.frames_out = m_framesOut;
        status.dropped = m_transport.dropped();
        status.bad_frames = m_badFrames;
        broadcast(message);
    }

    void HubApp::broadcastOta()
    {
        mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
        message.which_body = mark4_GatewayMessage_ota_state_tag;
        message.body.ota_state = otaStateOf(m_ota, m_otaTarget);
        broadcast(message);
    }

    void HubApp::housekeeping(std::uint64_t nowUs)
    {
        std::erase_if(m_rcSeenUs, [nowUs](const auto &entry) {
            return nowUs - entry.second > RC_PILOT_WINDOW_US;
        });
        m_ota.tick(nowUs);

        // A client that just connected knows nothing yet: it gets the table,
        // the counters and the update state as they stand.
        if (m_ws.takeConnectedFlag())
        {
            m_nodesDirty = true;
            broadcastStatus();
            broadcastOta();
        }
        if (nowUs >= m_nextStatusUs)
        {
            m_nextStatusUs = nowUs + STATUS_PERIOD_MS * US_PER_MS;
            m_nodesDirty = true;
            broadcastStatus();
        }
        if (m_nodesDirty)
        {
            broadcastNodes();
        }
    }
} // namespace mark4
