/// @file
/// @brief Stream health implementation.

#include "hub/stream_health.hpp"

#include <algorithm>

namespace mark4
{
    const char *streamKindName(StreamKind stream)
    {
        switch (stream)
        {
            case StreamKind::SIM_RAW:
                return "simRaw";
            case StreamKind::TRANSPORT:
                return "transport";
            case StreamKind::TELEMETRY:
                break;
        }
        return "telemetry";
    }

    void StreamHealth::onPacket(StreamKind stream, std::uint8_t sourceId, std::uint16_t sequence)
    {
        const auto found = std::find_if(
            m_links.begin(), m_links.end(), [stream, sourceId](const LinkHealth &link) {
                return link.stream == stream && link.sourceId == sourceId;
            });
        if (found == m_links.end())
        {
            LinkHealth link;
            link.stream = stream;
            link.sourceId = sourceId;
            link.received = 1U;
            link.lastSequence = sequence;
            m_links.push_back(link);
            return;
        }

        // The number is 16 bits and wraps, so the distance is read in that
        // same arithmetic: 65535 to 0 is one step forward, not a rewind.
        const auto delta = static_cast<std::uint16_t>(sequence - found->lastSequence);
        ++found->received;
        if (delta == 0U)
        {
            ++found->duplicates;
            return;
        }
        if (delta > RESYNC_THRESHOLD)
        {
            ++found->resyncs;
        }
        else
        {
            found->lost += delta - 1U;
        }
        found->lastSequence = sequence;
    }

    double linkLossRate(const LinkHealth &link)
    {
        const double expected = static_cast<double>(link.received) + static_cast<double>(link.lost);
        if (expected <= 0.0)
        {
            return 0.0;
        }
        return static_cast<double>(link.lost) / expected;
    }
} // namespace mark4
