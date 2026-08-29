/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains every source, routes every packet and keeps the discovery
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

#include "hub/packed_field.hpp"
#include "protocol/announce.hpp"
#include "protocol/tuning.hpp"
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
        // A bridge announces itself whether or not anyone listens, so the
        // port is bound for the whole run: the network tab of the pages then
        // has its list ready instead of building it after a click.
        if (!m_udp.subscribe(m_config.bridgePort))
        {
            return false;
        }
        // The hub sets no beacon: it learns every node from that node's own
        // beacon and is learnt in return from the first command it sends.
        if (!m_udpLink.init() || !m_transport.addLink(m_udpLink) || !m_transport.init())
        {
            static_cast<void>(std::fprintf(stderr, "hub: cannot start the transport\n"));
            return false;
        }
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
        // The plant's raw state is the one stream still outside the
        // transport: a plain broadcast port, watched for the whole run.
        if (!m_udp.subscribe(m_config.simRawPort))
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
        // as every other command, so an OTA packet reaches the board exactly
        // like an RC frame does, framed on the serial link.
        m_ota.setSink([this](const std::uint8_t *data, std::size_t size, std::string &errorOut) {
            return sendToTarget(m_otaTarget, data, size, errorOut);
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
            m_udp.appendPollFds(fds);
            for (const int fd : {m_udpLink.discoveryFd(), m_udpLink.dataFd()})
            {
                pollfd entry{};
                entry.fd = fd;
                entry.events = POLLIN;
                fds.push_back(entry);
            }
            if (m_serial.isOpen())
            {
                pollfd entry{};
                entry.fd = m_serial.fd();
                entry.events = POLLIN;
                fds.push_back(entry);
            }
            // A transfer is paced from this loop, so the loop has to come
            // round on the pacing scale rather than on the idle one.
            static_cast<void>(::poll(
                fds.data(), fds.size(), m_ota.busy() ? OTA_POLL_TIMEOUT_MS : POLL_TIMEOUT_MS));

            m_udp.drain([this](std::uint16_t port,
                               const UdpTransport::Source &from,
                               const std::uint8_t *data,
                               std::size_t size) { onDatagram(port, from, data, size); });
            m_transport.poll(monotonicUs(), &HubApp::OnFrame, this);
            m_serial.drain([this](const std::uint8_t *payload, std::size_t size) {
                onSerialPayload(payload, size);
            });
            handleClientMessages();
            housekeeping(monotonicUs());
        }
        return 0;
    }

    void HubApp::onDatagram(std::uint16_t localPort,
                            const UdpTransport::Source &from,
                            const std::uint8_t *data,
                            std::size_t size)
    {
        const std::uint64_t nowUs = monotonicUs();
        if (localPort == m_config.bridgePort)
        {
            if (m_bridges.onAnnounce(from.address, from.port, data, size, nowUs))
            {
                broadcastDiscovery();
            }
            return;
        }
        // Demultiplexing is by header, never by size: a packet whose version
        // or type byte does not match is simply not ours.
        if (hasHeader(data, size, PacketType::SIM_RAW) && size == SIM_RAW_PACKET_SIZE)
        {
            onSimRawPacket(data);
        }
        else
        {
            noteForeignProtocol(data, size);
        }
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
        const std::uint64_t nowUs = monotonicUs();
        if (hasHeader(data, size, PacketType::ANNOUNCE))
        {
            // The beacon of a flight process: the node it came from is the
            // address every command to that process goes to.
            const auto change = m_registry.onAnnounce(src, data, size, nowUs);
            if (change.has_value())
            {
                onDiscoveryChange(*change);
            }
            return;
        }
        if (hasHeader(data, size, PacketType::TELEMETRY) && size == TELEMETRY_PACKET_SIZE)
        {
            onTelemetryPacket(data);
        }
        else if (onTuningAnswer(data, size, StreamSource::DRONE_SIM))
        {
            // Tuning answers carry no source byte of their own, so the
            // arrival path is what names them: a payload from the transport
            // came from the simulator side, never from the board.
        }
        else if (!m_ota.onPacket(data, size, nowUs))
        {
            // Updater answers are one more packet type on whatever link the
            // process being updated is reachable on: a desktop flight process
            // answers over the transport, the board over the framed serial
            // link. Past the updater, nothing here knows these bytes.
            noteForeignProtocol(data, size);
        }
    }

    void HubApp::noteForeignProtocol(const std::uint8_t *data, std::size_t size)
    {
        if (size < 2U || data[0] == PROTOCOL_VERSION || data[0] == m_foreignProtocol)
        {
            return;
        }
        // A type byte in range is what separates a version mismatch from
        // unrelated traffic that happened to land on the port.
        if (data[1] == 0U || data[1] > static_cast<std::uint8_t>(PacketType::OTA_ACK))
        {
            return;
        }
        m_foreignProtocol = data[0];
        static_cast<void>(std::fprintf(
            stderr,
            "hub: dropping type %u packets, protocol %u != %u - a board flashed with another "
            "protocol version? it cannot be reached or updated from here (docs/ota-design.md)\n",
            static_cast<unsigned>(data[1]),
            static_cast<unsigned>(data[0]),
            static_cast<unsigned>(PROTOCOL_VERSION)));
    }

    void HubApp::onSerialPayload(const std::uint8_t *payload, std::size_t size)
    {
        if (hasHeader(payload, size, PacketType::TELEMETRY) && size == TELEMETRY_PACKET_SIZE)
        {
            const auto change = m_registry.onSerialTelemetry(monotonicUs());
            if (change.has_value())
            {
                onDiscoveryChange(*change);
            }
            onTelemetryPacket(payload);
            if (m_config.udpRebroadcast)
            {
                // The other transport nodes know nothing about this cable.
                static_cast<void>(m_transport.send(BROADCAST_NODE, payload, size));
            }
            return;
        }
        if (onTuningAnswer(payload, size, StreamSource::FIRMWARE))
        {
            return;
        }
        // OTA packets share this link with telemetry and commands: telemetry
        // keeps flowing between them, and the updater only ever sees its own.
        if (m_ota.onPacket(payload, size, monotonicUs()))
        {
            return;
        }
        noteForeignProtocol(payload, size);
        ++m_badFrames;
    }

    bool HubApp::onTuningAnswer(const std::uint8_t *data, std::size_t size, StreamSource source)
    {
        if (size == TUNING_ACK_PACKET_SIZE && hasHeader(data, size, PacketType::TUNING_ACK))
        {
            TuningAckPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            m_ws.broadcastText(tuningAckToJson(packet, source));
            return true;
        }
        if (size == TUNING_INFO_PACKET_SIZE && hasHeader(data, size, PacketType::TUNING_INFO))
        {
            TuningInfoPacket packet{};
            std::memcpy(&packet, data, sizeof(packet));
            m_ws.broadcastText(tuningInfoToJson(packet, source));
            return true;
        }
        return false;
    }

    bool HubApp::sendToTarget(StreamSource target,
                              const std::uint8_t *data,
                              std::size_t size,
                              std::string &errorOut)
    {
        if (target == StreamSource::FIRMWARE)
        {
            if (!m_serial.isOpen())
            {
                errorOut = "no serial link to the board";
                return false;
            }
            return m_serial.sendPacket(data, size);
        }
        const std::uint32_t nodeId = m_registry.nodeIdOf(target);
        if (nodeId == 0U)
        {
            errorOut = std::string("no process of kind ") + streamSourceName(target);
            return false;
        }
        if (!m_transport.send(nodeId, data, size))
        {
            errorOut =
                std::string("transport node of ") + streamSourceName(target) + " is not reachable";
            return false;
        }
        return true;
    }

    void HubApp::onTelemetryPacket(const std::uint8_t *data)
    {
        TelemetryPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        m_health.onPacket(StreamKind::TELEMETRY, packet.sourceId, packet.sequence);

        m_ws.broadcastText(telemetryToJson(packet));
    }

    void HubApp::onSimRawPacket(const std::uint8_t *data)
    {
        SimRawPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        m_health.onPacket(StreamKind::SIM_RAW, packet.sourceId, packet.sequence);

        m_ws.broadcastText(simRawToJson(packet));
    }

    void HubApp::onDiscoveryChange(const DiscoveryChange &change)
    {
        switch (change.event)
        {
            case DiscoveryEvent::APPEARED:
                static_cast<void>(std::printf("hub: %s appeared (node %u)\n",
                                              streamSourceName(change.process.kind),
                                              change.process.nodeId));
                break;
            case DiscoveryEvent::RESTARTED:
                static_cast<void>(std::printf("hub: %s restarted (node %u)\n",
                                              streamSourceName(change.process.kind),
                                              change.process.nodeId));
                break;
            case DiscoveryEvent::DISAPPEARED:
                static_cast<void>(
                    std::printf("hub: %s disappeared\n", streamSourceName(change.process.kind)));
                break;
        }
        if (!m_config.pushProfileName.empty() && change.event != DiscoveryEvent::DISAPPEARED)
        {
            // A flight process has no flash: it boots on the defaults every
            // time. Pushing on the announce is what makes a bench session
            // survive a restart of the thing being tuned.
            std::string error;
            if (pushProfile(m_config.pushProfileName, change.process.kind, error))
            {
                static_cast<void>(std::printf("hub: pushed profile %s to %s\n",
                                              m_config.pushProfileName.c_str(),
                                              streamSourceName(change.process.kind)));
            }
            else
            {
                static_cast<void>(std::fprintf(stderr,
                                               "hub: cannot push profile %s to %s: %s\n",
                                               m_config.pushProfileName.c_str(),
                                               streamSourceName(change.process.kind),
                                               error.c_str()));
            }
        }
        static_cast<void>(std::fflush(stdout));
        broadcastDiscovery();
    }

    bool HubApp::pushProfile(const std::string &name, StreamSource target, std::string &errorOut)
    {
        TuningValues values;
        if (!m_profiles.load(name, values, errorOut))
        {
            return false;
        }
        for (const auto &[id, value] : values)
        {
            TuningSetPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::TUNING_SET);
            packet.id = id;
            packet.value = value;
            const auto bytes = wireBytes(packet);
            if (!sendToTarget(target, bytes.data(), bytes.size(), errorOut))
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

    bool HubApp::commandAllowed(StreamSource target, std::string &errorOut) const
    {
        if (m_connection.via.empty())
        {
            errorOut = "no drone connected";
            return false;
        }
        if (target != m_connection.kind)
        {
            errorOut = std::string("connected to ") + m_connection.id + ", not to " +
                       streamSourceName(target);
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
            case ClientMessageType::RC: {
                // The hub forwards RC one for one and never repeats or
                // synthesizes it: a silent link means kill downstream, and
                // that silence is the pilot's, not the hub's to fill in.
                const auto bytes = wireBytes(message.rc);
                return sendToTarget(message.target, bytes.data(), bytes.size(), errorOut);
            }
            case ClientMessageType::TUNING_SET: {
                const auto bytes = wireBytes(message.tuningSet);
                return sendToTarget(message.target, bytes.data(), bytes.size(), errorOut);
            }
            case ClientMessageType::TUNING_LIST: {
                // The ack the client gets back says the request went out, not
                // that the table arrived: the descriptions come as their own
                // messages, one per flight frame, as the process unrolls them.
                const auto bytes = wireBytes(message.tuningList);
                return sendToTarget(message.target, bytes.data(), bytes.size(), errorOut);
            }
            case ClientMessageType::SIM_SCENARIO: {
                SimScenarioPacket packet = message.simScenario;
                if (packet.scenario.sequence == 0U)
                {
                    // 0 means "no scenario" on the wire, so a client that
                    // sent none gets the hub's own rolling number: two
                    // scenarios in a row are then two scenarios, not one.
                    m_scenarioSequence =
                        static_cast<std::uint8_t>(m_scenarioSequence % MAX_SCENARIO_SEQUENCE + 1U);
                    packet.scenario.sequence = m_scenarioSequence;
                }
                // Routed like every other command: to the port the target
                // announced. The plant binds nothing and the hub hardwires
                // no port; the flight process forwards the block from there.
                const auto bytes = wireBytes(packet);
                return sendToTarget(message.target, bytes.data(), bytes.size(), errorOut);
            }
            case ClientMessageType::REBOOT: {
                if (message.target != StreamSource::FIRMWARE)
                {
                    errorOut = std::string("cannot reboot ") + streamSourceName(message.target);
                    return false;
                }
                const auto bytes = wireBytes(message.reboot);
                return sendToTarget(message.target, bytes.data(), bytes.size(), errorOut);
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
        Connection next;
        next.via = message.connectVia;
        if (message.connectVia == "udp")
        {
            if (message.target == StreamSource::FIRMWARE)
            {
                errorOut = "the board is reached over a bridge, not udp";
                return false;
            }
            next.id = streamSourceName(message.target);
            next.kind = message.target;
        }
        else
        {
            // The bridge told the network where it is; the operator only
            // ever names it. Its address is resolved here and re-resolved
            // by refreshConnection() if the router later hands out another.
            const std::vector<DiscoveredBridge> &bridges = m_bridges.bridges();
            const auto found = std::find_if(
                bridges.begin(), bridges.end(), [&message](const DiscoveredBridge &bridge) {
                    return bridge.name == message.connectPeer;
                });
            if (found == bridges.end())
            {
                errorOut = "no bridge named '" + message.connectPeer + "' on the network";
                return false;
            }
            const std::string device = std::string(SerialTransport::UDP_PREFIX) + found->address +
                                       ":" + std::to_string(found->port);
            if (!m_serial.open(device))
            {
                // A failed open still remembers the device for the periodic
                // retry; a refused connect must leave no such ghost behind.
                m_serial.release();
                errorOut = "cannot open " + device;
                return false;
            }
            next.id = message.connectPeer;
        }
        if (next.via == "udp" && m_serial.isOpen())
        {
            // Connecting is exclusive: the serial link belonged to the
            // previous connection and nobody is on it any more.
            m_serial.release();
        }
        m_connection = next;
        refreshConnection();
        broadcastStatus();
        return true;
    }

    void HubApp::applyDisconnect()
    {
        if (m_connection.via == "bridge")
        {
            m_serial.release();
        }
        m_connection = Connection{};
        broadcastStatus();
    }

    void HubApp::refreshConnection()
    {
        if (m_connection.via.empty())
        {
            return;
        }
        if (m_connection.via == "bridge")
        {
            // Follow a bridge whose router handed out a new address: the
            // name is the identity, the address is just today's route to it.
            const std::vector<DiscoveredBridge> &bridges = m_bridges.bridges();
            const auto found = std::find_if(
                bridges.begin(), bridges.end(), [this](const DiscoveredBridge &bridge) {
                    return bridge.name == m_connection.id;
                });
            if (found != bridges.end())
            {
                const std::string device = std::string(SerialTransport::UDP_PREFIX) +
                                           found->address + ":" + std::to_string(found->port);
                if (m_serial.device() != device)
                {
                    static_cast<void>(m_serial.open(device));
                }
            }
        }
        const bool viaSerial = m_connection.via != "udp";
        const auto &processes = m_registry.processes();
        const bool live = std::any_of(processes.begin(),
                                      processes.end(),
                                      [this, viaSerial](const DiscoveredProcess &process) {
                                          return process.kind == m_connection.kind &&
                                                 process.viaSerial == viaSerial;
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
        m_ws.broadcastText(
            discoveryToJson(m_registry.processes(), m_bridges.bridges(), monotonicUs()));
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
        if (m_bridges.expire(nowUs, DISCOVERY_EXPIRY_US) > 0U)
        {
            broadcastDiscovery();
        }
        m_serial.maintain(nowUs / US_PER_MS);
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
        snapshot.serialOpen = m_serial.isOpen();
        snapshot.serialLink = m_serial.isOpen() ? m_serial.device() : std::string{};
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
        snapshot.links = m_health.links();
        for (LinkHealth &link : snapshot.links)
        {
            // The counters know a source byte; only the discovery table knows
            // what that byte is called, and only while the process is alive.
            const auto found =
                std::find_if(m_registry.processes().begin(),
                             m_registry.processes().end(),
                             [&link](const DiscoveredProcess &process) {
                                 return static_cast<std::uint8_t>(process.kind) == link.sourceId;
                             });
            if (found != m_registry.processes().end())
            {
                link.sourceName = streamSourceName(found->kind);
            }
        }
        // One more entry per process reached over the transport: the frame
        // counters of its node, every payload type included.
        for (const DiscoveredProcess &process : m_registry.processes())
        {
            const Transport::Node *node = m_transport.findNode(process.nodeId);
            if (node == nullptr)
            {
                continue;
            }
            LinkHealth link;
            link.stream = StreamKind::TRANSPORT;
            link.sourceId = static_cast<std::uint8_t>(process.kind);
            link.sourceName = streamSourceName(process.kind);
            link.received = node->received;
            link.lost = node->lost;
            link.duplicates = node->duplicates;
            link.lastSequence = node->lastSeq;
            snapshot.links.push_back(link);
        }
        return snapshot;
    }
} // namespace mark4
