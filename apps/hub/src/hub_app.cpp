/// @file
/// @brief hub composition root implementation: the single poll loop that
///        drains every source, routes every packet and keeps the discovery
///        table honest.

#include "hub/hub_app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <utility>
#include <variant>

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
        if (!m_ws.start(m_config.wsPort))
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
            static_cast<void>(::poll(fds.data(), fds.size(), POLL_TIMEOUT_MS));

            m_udp.drain([this](std::uint16_t port, const std::uint8_t *data, std::size_t size) {
                onDatagram(port, data, size);
            });
            m_serial.drain([this](const std::uint8_t *payload, std::size_t size) {
                onSerialPayload(payload, size);
            });
            handleClientMessages();
            housekeeping(monotonicUs());
        }
        return 0;
    }

    void HubApp::onDatagram(std::uint16_t localPort, const std::uint8_t *data, std::size_t size)
    {
        const std::uint64_t nowUs = monotonicUs();
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
        else
        {
            // Tuning answers ride the telemetry stream: they carry no source
            // byte of their own, so the arrival path is what names them. A
            // datagram landing on a UDP telemetry port came from the
            // simulator side, never from the board.
            static_cast<void>(onTuningAnswer(data, size, StreamSource::DRONE_SIM));
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
                // The ground station and the simulator ghost view listen on
                // UDP and know nothing about this cable.
                static_cast<void>(m_udp.broadcast(payload, size, m_config.telemetryPort));
                static_cast<void>(
                    m_udp.broadcast(payload, size, telemetryMirrorPort(m_config.telemetryPort)));
            }
            return;
        }
        if (onTuningAnswer(payload, size, StreamSource::FIRMWARE))
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
        m_recorder.onTelemetry(packet);
        m_ws.broadcastText(telemetryToJson(packet));
    }

    void HubApp::onSimRawPacket(const std::uint8_t *data)
    {
        SimRawPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        m_recorder.onSimRaw(packet);
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
        for (const std::string &text : m_ws.drainInbound())
        {
            const auto decoded = parseClientMessage(text);
            if (const auto *reason = std::get_if<std::string>(&decoded))
            {
                answer(clientMessageId(text), false, *reason);
                continue;
            }
            const auto &message = std::get<ClientMessage>(decoded);
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
            case ClientMessageType::SIM_COMMAND: {
                const auto bytes = wireBytes(message.simCommand);
                return m_udp.sendTo(
                    bytes.data(), bytes.size(), "127.0.0.1", m_config.simCommandPort);
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
        }
        errorOut = "unsupported request";
        return false;
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

    void HubApp::housekeeping(std::uint64_t nowUs)
    {
        for (const DiscoveryChange &change : m_registry.expire(nowUs, DISCOVERY_EXPIRY_US))
        {
            onDiscoveryChange(change);
        }
        m_serial.maintain(nowUs / US_PER_MS);

        // A client that just connected knows nothing yet: it gets the table
        // and the counters as they stand, without waiting for a change.
        if (m_ws.takeConnectedFlag())
        {
            broadcastDiscovery();
            broadcastStatus();
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
        snapshot.telemetryRows = stats.telemetryRows;
        snapshot.simRawRows = stats.simRawRows;
        snapshot.blackboxRecords = stats.blackboxRecords;
        snapshot.badFrames = stats.badFrames;
        snapshot.rejectedAnnounces = m_registry.rejectedAnnounces();
        snapshot.clients = m_ws.clientCount();
        return snapshot;
    }
} // namespace mark4
