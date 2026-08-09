/// @file
/// @brief Discovery registry implementation.

#include "hub/discovery.hpp"

#include <algorithm>
#include <cstring>

namespace mark4
{
    const char *streamSourceName(StreamSource kind)
    {
        switch (kind)
        {
            case StreamSource::FIRMWARE:
                return "firmware";
            case StreamSource::DRONE_SIM:
                return "drone_sim";
            case StreamSource::DRONE_REPLAY:
                return "drone_replay";
            case StreamSource::SIM_PLANT:
                return "sim_plant";
        }
        return "unknown";
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::onAnnounce(const std::uint8_t *data,
                                                                 std::size_t size,
                                                                 std::uint64_t nowUs)
    {
        // Everything landing on the announce port is meant to be an announce,
        // so anything else is a real mismatch worth counting: an emitter left
        // behind on an older wire version shows up here and nowhere else.
        if (size != ANNOUNCE_PACKET_SIZE || !hasHeader(data, size, PacketType::ANNOUNCE))
        {
            ++m_rejectedAnnounces;
            return std::nullopt;
        }

        AnnouncePacket packet{};
        std::memcpy(&packet, data, sizeof(packet));

        DiscoveredProcess candidate;
        candidate.kind = static_cast<StreamSource>(packet.kind);
        candidate.sessionId = packet.sessionId;
        candidate.telemetryPort = packet.telemetryPort;
        candidate.commandPort = packet.commandPort;
        candidate.lastSeenUs = nowUs;
        candidate.viaSerial = false;
        return touch(candidate);
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::onSerialTelemetry(std::uint64_t nowUs)
    {
        DiscoveredProcess candidate;
        candidate.kind = StreamSource::FIRMWARE;
        candidate.sessionId = 0U;
        candidate.telemetryPort = 0U;
        candidate.commandPort = 0U;
        candidate.lastSeenUs = nowUs;
        candidate.viaSerial = true;
        return touch(candidate);
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::touch(const DiscoveredProcess &candidate)
    {
        const auto same = [&candidate](const DiscoveredProcess &known) {
            return known.kind == candidate.kind && known.telemetryPort == candidate.telemetryPort &&
                   known.commandPort == candidate.commandPort;
        };
        const auto found = std::find_if(m_processes.begin(), m_processes.end(), same);
        if (found == m_processes.end())
        {
            m_processes.push_back(candidate);
            return DiscoveryChange{DiscoveryEvent::APPEARED, candidate};
        }

        // A session identity of 0 means "not assigned": it can never prove a
        // restart, so such an announce is always a plain refresh.
        const bool restarted = candidate.sessionId != 0U && found->sessionId != candidate.sessionId;
        *found = candidate;
        if (restarted)
        {
            return DiscoveryChange{DiscoveryEvent::RESTARTED, candidate};
        }
        return std::nullopt;
    }

    std::vector<DiscoveryChange> DiscoveryRegistry::expire(std::uint64_t nowUs,
                                                           std::uint64_t expiryUs)
    {
        std::vector<DiscoveryChange> changes;
        auto stale = m_processes.begin();
        while (stale != m_processes.end())
        {
            if (nowUs >= stale->lastSeenUs && nowUs - stale->lastSeenUs >= expiryUs)
            {
                changes.push_back(DiscoveryChange{DiscoveryEvent::DISAPPEARED, *stale});
                stale = m_processes.erase(stale);
            }
            else
            {
                ++stale;
            }
        }
        return changes;
    }

    std::uint16_t DiscoveryRegistry::commandPortOf(StreamSource kind) const
    {
        for (const DiscoveredProcess &known : m_processes)
        {
            if (known.kind == kind && known.commandPort != 0U)
            {
                return known.commandPort;
            }
        }
        return 0U;
    }
} // namespace mark4
