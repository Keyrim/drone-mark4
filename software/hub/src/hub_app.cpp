/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains every source, routes every packet and keeps the discovery
///        table honest.

#include "hub/hub_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "hub/launcher.hpp"
#include "hub/packed_field.hpp"
#include "hub/recordings.hpp"
#include "protocol/blackbox.hpp"
#include "protocol/tuning.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;

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
          m_recorder(m_config.logDirectory),
          m_profiles(m_config.profilesDir)
    {
    }

    bool HubApp::init()
    {
        if (!m_udp.init(m_config.announcePort))
        {
            return false;
        }
        // A bridge announces itself whether or not anyone listens, so the
        // port is bound for the whole run: the network tab of the pages then
        // has its list ready instead of building it after a click.
        if (!m_udp.subscribe(m_config.bridgePort))
        {
            return false;
        }
        if (m_config.pagesDir.empty())
        {
            m_config.pagesDir = defaultProjectPath(DEFAULT_PAGES_DIR);
        }
        std::error_code failure;
        if (!std::filesystem::is_directory(m_config.pagesDir, failure))
        {
            // A hub without pages still decodes, records and serves the
            // websocket: the pages are a client, not a dependency.
            static_cast<void>(
                std::printf("hub: no pages in %s, serving none\n", m_config.pagesDir.c_str()));
        }
        HttpConfig http;
        http.pagesDir = m_config.pagesDir;
        http.logDir = m_config.logDirectory;
        if (!m_ws.start(m_config.wsPort, m_config.bindAddress, std::move(http)))
        {
            return false;
        }
        // The configured stream ports are watched from the start and never
        // released: the hub has to be useful before any announce arrives,
        // and a process that never announces itself still gets decoded.
        for (const std::uint16_t port : {m_config.telemetryPort, m_config.simRawPort})
        {
            if (!m_udp.subscribe(port))
            {
                return false;
            }
            if (std::none_of(m_followedPorts.begin(),
                             m_followedPorts.end(),
                             [port](const PortUse &use) { return use.port == port; }))
            {
                m_followedPorts.push_back(PortUse{port, 0U});
            }
        }

        if (!m_config.serialDevice.empty())
        {
            // A board that is not plugged in yet is not a startup failure:
            // the link is retried for as long as the hub runs.
            static_cast<void>(m_serial.open(m_config.serialDevice, m_config.serialBaud));
        }

        if (m_config.otaBundlePath.empty())
        {
            // The bundle is a build artifact: resolving its path must not
            // depend on the build having happened yet.
            m_config.otaBundlePath = defaultProjectPath(DEFAULT_OTA_BUNDLE, false);
        }
        m_ota.setDefaultBundlePath(m_config.otaBundlePath);
        // The update client owns no socket: it sends through the same routing
        // as every other command, so an OTA packet reaches the board exactly
        // like an RC frame does, framed on the serial link.
        m_ota.setSink([this](const std::uint8_t *data, std::size_t size, std::string &errorOut) {
            return sendToTarget(m_otaTarget, data, size, errorOut);
        });
        m_ota.setOnChange([this]() { broadcastOta(); });

        if (m_config.recordOnStart && !m_recorder.startCsvSession())
        {
            static_cast<void>(std::fprintf(
                stderr, "hub: cannot open a recording in %s\n", m_config.logDirectory.c_str()));
            return false;
        }
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
        if (localPort == m_config.announcePort)
        {
            const auto change = m_registry.onAnnounce(data, size, nowUs);
            if (change.has_value())
            {
                onDiscoveryChange(*change);
            }
            return;
        }

        // Demultiplexing is by header, never by size: a packet whose version
        // or type byte does not match is simply not ours.
        if (hasHeader(data, size, PacketType::TELEMETRY) && size == TELEMETRY_PACKET_SIZE)
        {
            // With the serial rebroadcast on, the board telemetry arriving
            // over UDP is the hub's own echo of what it just re-emitted.
            if (m_config.udpRebroadcast && m_serial.isOpen() &&
                data[2] == static_cast<std::uint8_t>(StreamSource::FIRMWARE))
            {
                return;
            }
            onTelemetryPacket(data);
        }
        else if (hasHeader(data, size, PacketType::SIM_RAW) && size == SIM_RAW_PACKET_SIZE)
        {
            onSimRawPacket(data);
        }
        else if (onTuningAnswer(data, size, StreamSource::DRONE_SIM))
        {
            // Tuning answers ride the telemetry stream: they carry no source
            // byte of their own, so the arrival path is what names them. A
            // datagram landing on a UDP telemetry port came from the
            // simulator side, never from the board.
        }
        else
        {
            // Updater answers are one more packet type on whatever link the
            // process being updated is reachable on: a desktop flight process
            // answers over UDP, the board over the framed serial link.
            static_cast<void>(m_ota.onPacket(data, size, nowUs));
        }
    }

    void HubApp::onSerialPayload(const std::uint8_t *payload, std::size_t size)
    {
        if (size == BLACKBOX_RECORD_SIZE && validBlackboxRecord(payload))
        {
            static_cast<void>(m_recorder.onBlackboxRecord(payload, size));
            return;
        }
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
                // UDP listeners know nothing about this cable.
                static_cast<void>(m_udp.broadcast(payload, size, m_config.telemetryPort));
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
        m_recorder.countBadFrame();
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
        const std::uint16_t port = m_registry.commandPortOf(target);
        if (port == 0U)
        {
            errorOut = std::string("no process of kind ") + streamSourceName(target);
            return false;
        }
        return m_udp.sendTo(data, size, "127.0.0.1", port);
    }

    void HubApp::onTelemetryPacket(const std::uint8_t *data)
    {
        TelemetryPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        m_health.onPacket(StreamKind::TELEMETRY, packet.sourceId, packet.sequence);
        m_recorder.onTelemetry(packet);

        const auto quaternion = readPackedField(&packet.attitudeQuat);
        AlignSample sample;
        sample.timestampUs = static_cast<double>(packet.timestampUs);
        for (std::size_t index = 0U; index < quaternion.size(); ++index)
        {
            sample.attitudeQuat[index] = static_cast<double>(quaternion[index]);
        }
        sample.altitudeM = static_cast<double>(packet.altitudeM);
        sample.verticalVelocityMps = static_cast<double>(packet.verticalVelocityMps);
        m_aligner.onTelemetry(sample);

        m_ws.broadcastText(telemetryToJson(packet));
    }

    void HubApp::onSimRawPacket(const std::uint8_t *data)
    {
        SimRawPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        m_health.onPacket(StreamKind::SIM_RAW, packet.sourceId, packet.sequence);
        m_recorder.onSimRaw(packet);

        const auto quaternion = readPackedField(&packet.attitudeQuat);
        const auto position = readPackedField(&packet.positionM);
        const auto velocity = readPackedField(&packet.velocityMps);
        AlignSample sample;
        sample.timestampUs = static_cast<double>(packet.timestampUs);
        for (std::size_t index = 0U; index < quaternion.size(); ++index)
        {
            sample.attitudeQuat[index] = static_cast<double>(quaternion[index]);
        }
        // The exact altitude is the world z of the position, the exact
        // vertical speed the world z of the velocity, exactly as the sim raw
        // CSV columns the offline comparison reads.
        sample.altitudeM = static_cast<double>(position[2]);
        sample.verticalVelocityMps = static_cast<double>(velocity[2]);
        m_aligner.onSimRaw(sample);

        m_ws.broadcastText(simRawToJson(packet));
    }

    void HubApp::onDiscoveryChange(const DiscoveryChange &change)
    {
        switch (change.event)
        {
            case DiscoveryEvent::APPEARED:
                retainPort(change.process.telemetryPort);
                static_cast<void>(
                    std::printf("hub: %s appeared (telemetry udp/%u, command udp/%u)\n",
                                streamSourceName(change.process.kind),
                                static_cast<unsigned>(change.process.telemetryPort),
                                static_cast<unsigned>(change.process.commandPort)));
                break;
            case DiscoveryEvent::RESTARTED:
                static_cast<void>(std::printf("hub: %s restarted (session %u)\n",
                                              streamSourceName(change.process.kind),
                                              change.process.sessionId));
                break;
            case DiscoveryEvent::DISAPPEARED:
                releasePort(change.process.telemetryPort);
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

    bool HubApp::startReplay(const std::string &name,
                             const std::string &speed,
                             std::string &errorOut)
    {
        Recording recording;
        if (!findRecording(listRecordings(m_config.logDirectory), name, recording))
        {
            errorOut = "no recording named '" + name + "'";
            return false;
        }
        if (recording.kind != "blackbox")
        {
            // A streams pair is two CSV files of what a run published; only
            // a blackbox holds the sensor frames a replay steps through.
            errorOut = "'" + name + "' is a streams recording, not a blackbox";
            return false;
        }

        // One replay at a time: two of them would broadcast telemetry on the
        // same port and the ground side would read one interleaved run.
        static_cast<void>(m_replays.terminateAll());

        ChildSpec child;
        child.program = defaultBinaryPath("drone_replay");
        child.arguments.push_back(recordingDirectory(m_config.logDirectory, recording) + "/" +
                                  recording.name);
        if (!speed.empty())
        {
            child.arguments.emplace_back("--speed");
            child.arguments.push_back(speed);
        }
        child.arguments.emplace_back("--announce-port");
        child.arguments.push_back(std::to_string(m_config.announcePort));
        if (!m_replays.spawn(child))
        {
            errorOut = "cannot start " + child.program;
            return false;
        }
        return true;
    }

    void HubApp::retainPort(std::uint16_t port)
    {
        if (port == 0U)
        {
            return;
        }
        const auto found = std::find_if(m_followedPorts.begin(),
                                        m_followedPorts.end(),
                                        [port](const PortUse &use) { return use.port == port; });
        if (found != m_followedPorts.end())
        {
            ++found->users;
            return;
        }
        if (m_udp.subscribe(port))
        {
            m_followedPorts.push_back(PortUse{port, 1U});
        }
    }

    void HubApp::releasePort(std::uint16_t port)
    {
        if (port == 0U)
        {
            return;
        }
        const auto found = std::find_if(m_followedPorts.begin(),
                                        m_followedPorts.end(),
                                        [port](const PortUse &use) { return use.port == port; });
        if (found == m_followedPorts.end() || found->users == 0U)
        {
            // Either unknown, or one of the ports the configuration pinned:
            // those stay subscribed for the whole run.
            return;
        }
        --found->users;
        if (found->users == 0U)
        {
            m_udp.unsubscribe(port);
            static_cast<void>(m_followedPorts.erase(found));
        }
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

    bool HubApp::applyClientMessage(const ClientMessage &message, std::string &errorOut)
    {
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
            case ClientMessageType::TUNING_GET: {
                const auto bytes = wireBytes(message.tuningGet);
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
            case ClientMessageType::REPLAY: {
                return startReplay(message.recordingName, message.replaySpeed, errorOut);
            }
            case ClientMessageType::SERIAL: {
                if (message.serialConnect)
                {
                    if (!m_serial.open(message.serialDevice, message.serialBaud))
                    {
                        errorOut = "cannot open " + message.serialDevice;
                        return false;
                    }
                }
                else
                {
                    m_serial.release();
                }
                broadcastStatus();
                return true;
            }
            case ClientMessageType::RECORD: {
                if (message.recordStart)
                {
                    if (!m_recorder.startCsvSession())
                    {
                        errorOut = "cannot open a recording in " + m_config.logDirectory;
                        return false;
                    }
                }
                else
                {
                    m_recorder.stopCsvSession();
                }
                broadcastStatus();
                return true;
            }
            case ClientMessageType::OTA_STATUS:
            case ClientMessageType::OTA_START:
            case ClientMessageType::OTA_ABORT:
            case ClientMessageType::OTA_CONFIRM:
            case ClientMessageType::OTA_REVERT:
            case ClientMessageType::OTA_CONFIG: {
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
            case ClientMessageType::OTA_CONFIRM:
                return m_ota.confirm(nowUs, errorOut);
            case ClientMessageType::OTA_REVERT:
                return m_ota.revert(nowUs, errorOut);
            case ClientMessageType::OTA_CONFIG:
                m_ota.setAutoConfirm(message.otaAutoConfirm);
                return true;
            default:
                errorOut = "unsupported update request";
                return false;
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

    void HubApp::emitCompare()
    {
        for (const AlignedPair &pair : m_aligner.takeDue())
        {
            m_ws.broadcastText(compareToJson(pair));
        }
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
        std::erase_if(m_rcSeenUs, [nowUs](const auto &entry) {
            return nowUs - entry.second > RC_PILOT_WINDOW_US;
        });
        // A replay that reached the end of its file must be reaped, or it
        // stays a zombie for as long as the hub runs.
        static_cast<void>(m_replays.anyExited());
        m_ota.tick(nowUs);
        emitCompare();

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
        const StreamRecorder::Stats &stats = m_recorder.stats();
        HubStatus snapshot;
        snapshot.recording = m_recorder.csvSessionOpen();
        snapshot.serialOpen = m_serial.isOpen();
        snapshot.serialLink = m_serial.isOpen() ? m_serial.device() : std::string{};
        snapshot.telemetryRows = stats.telemetryRows;
        snapshot.simRawRows = stats.simRawRows;
        snapshot.blackboxRecords = stats.blackboxRecords;
        snapshot.badFrames = stats.badFrames;
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
        return snapshot;
    }
} // namespace mark4
