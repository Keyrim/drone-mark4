/// @file
/// @brief Discovery registry implementation.

#include "hub/discovery.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

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
            case StreamSource::SIM_PLANT:
                return "sim_plant";
        }
        return "unknown";
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::onAnnounce(std::uint32_t nodeId,
                                                                 const std::uint8_t *data,
                                                                 std::size_t size,
                                                                 std::uint64_t nowUs)
    {
        // The caller hands over what looks like an announce, so anything
        // else is a real mismatch worth counting: an emitter left behind on
        // an older wire version shows up here and nowhere else.
        if (nodeId == 0U || size != ANNOUNCE_PACKET_SIZE ||
            !hasHeader(data, size, PacketType::ANNOUNCE))
        {
            ++m_rejectedAnnounces;
            return std::nullopt;
        }

        AnnouncePacket packet{};
        std::memcpy(&packet, data, sizeof(packet));

        DiscoveredProcess candidate;
        candidate.kind = static_cast<StreamSource>(packet.kind);
        candidate.nodeId = nodeId;
        candidate.lastSeenUs = nowUs;
        candidate.viaSerial = false;
        return touch(candidate);
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::onSerialTelemetry(std::uint64_t nowUs)
    {
        DiscoveredProcess candidate;
        candidate.kind = StreamSource::FIRMWARE;
        candidate.nodeId = 0U;
        candidate.lastSeenUs = nowUs;
        candidate.viaSerial = true;
        return touch(candidate);
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::touch(const DiscoveredProcess &candidate)
    {
        const auto same = [&candidate](const DiscoveredProcess &known) {
            return known.kind == candidate.kind && known.viaSerial == candidate.viaSerial;
        };
        const auto found = std::find_if(m_processes.begin(), m_processes.end(), same);
        if (found == m_processes.end())
        {
            m_processes.push_back(candidate);
            return DiscoveryChange{DiscoveryEvent::APPEARED, candidate};
        }

        // A node identity of 0 is the serial route, which carries none: it
        // can never prove a restart, so such evidence is always a plain
        // refresh.
        const bool restarted = candidate.nodeId != 0U && found->nodeId != candidate.nodeId;
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

    bool BridgeDirectory::onAnnounce(const char *address,
                                     std::uint16_t port,
                                     const std::uint8_t *data,
                                     std::size_t size,
                                     std::uint64_t nowUs)
    {
        if (address == nullptr)
        {
            return false;
        }
        const std::string_view text(reinterpret_cast<const char *>(data), size);
        const std::string_view word(WORD);
        if (!text.starts_with(word))
        {
            return false;
        }
        std::string name;
        for (const char letter : text.substr(word.size()))
        {
            const bool keep = (letter >= '0' && letter <= '9') ||
                              (letter >= 'a' && letter <= 'z') ||
                              (letter >= 'A' && letter <= 'Z') || letter == '-';
            if (keep && name.size() < MAX_NAME)
            {
                name.push_back(letter);
            }
        }

        for (DiscoveredBridge &known : m_bridges)
        {
            if (known.address == address && known.port == port)
            {
                known.name = name;
                known.lastSeenUs = nowUs;
                return false;
            }
        }
        m_bridges.push_back(DiscoveredBridge{address, port, name, nowUs});
        return true;
    }

    std::size_t BridgeDirectory::expire(std::uint64_t nowUs, std::uint64_t expiryUs)
    {
        const std::size_t before = m_bridges.size();
        std::erase_if(m_bridges, [nowUs, expiryUs](const DiscoveredBridge &known) {
            return nowUs >= known.lastSeenUs && nowUs - known.lastSeenUs >= expiryUs;
        });
        return before - m_bridges.size();
    }

    std::uint32_t DiscoveryRegistry::nodeIdOf(StreamSource kind) const
    {
        for (const DiscoveredProcess &known : m_processes)
        {
            if (known.kind == kind && known.nodeId != 0U)
            {
                return known.nodeId;
            }
        }
        return 0U;
    }
} // namespace mark4
