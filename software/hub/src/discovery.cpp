/// @file
/// @brief Discovery registry implementation.

#include "hub/discovery.hpp"

#include <algorithm>

namespace mark4
{
    const char *nodeKindName(mark4_NodeKind kind)
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
            case mark4_NodeKind_NODE_KIND_UNSPECIFIED:
                break;
        }
        return "unknown";
    }

    bool parseNodeKindName(const std::string &name, mark4_NodeKind &kindOut)
    {
        for (const mark4_NodeKind kind : {mark4_NodeKind_FIRMWARE,
                                          mark4_NodeKind_DRONE_SIM,
                                          mark4_NodeKind_PLANT,
                                          mark4_NodeKind_GATEWAY,
                                          mark4_NodeKind_BATCH})
        {
            if (name == nodeKindName(kind))
            {
                kindOut = kind;
                return true;
            }
        }
        return false;
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::onAnnounce(std::uint32_t nodeId,
                                                                 const mark4_Announce &announce,
                                                                 std::uint64_t nowUs)
    {
        if (announce.kind == mark4_NodeKind_NODE_KIND_UNSPECIFIED)
        {
            ++m_rejectedAnnounces;
            return std::nullopt;
        }

        DiscoveredProcess candidate;
        candidate.kind = announce.kind;
        candidate.nodeId = nodeId;
        candidate.lastSeenUs = nowUs;
        candidate.name = announce.name;
        candidate.mcu = announce.mcu;
        candidate.buildEpoch = announce.build_epoch;
        candidate.gitHash = announce.git_hash;
        candidate.wireHash = announce.wire_hash;
        candidate.wireMismatch = announce.wire_hash != WIRE_HASH;
        return touch(candidate);
    }

    std::optional<DiscoveryChange> DiscoveryRegistry::touch(const DiscoveredProcess &candidate)
    {
        const auto same = [&candidate](const DiscoveredProcess &known) {
            return known.kind == candidate.kind;
        };
        const auto found = std::find_if(m_processes.begin(), m_processes.end(), same);
        if (found == m_processes.end())
        {
            m_processes.push_back(candidate);
            return DiscoveryChange{DiscoveryEvent::APPEARED, candidate};
        }

        const bool restarted = found->nodeId != candidate.nodeId;
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

    std::uint32_t DiscoveryRegistry::nodeIdOf(mark4_NodeKind kind) const
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

    bool DiscoveryRegistry::kindOf(std::uint32_t nodeId, mark4_NodeKind &kindOut) const
    {
        for (const DiscoveredProcess &known : m_processes)
        {
            if (nodeId != 0U && known.nodeId == nodeId)
            {
                kindOut = known.kind;
                return true;
            }
        }
        return false;
    }
} // namespace mark4
