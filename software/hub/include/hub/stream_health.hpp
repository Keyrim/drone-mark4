#pragma once

/// @file
/// @brief Per-link health of the decoded streams, read from the sequence
///        number every stream packet carries. Pure logic: no socket, no
///        clock, so the whole behavior is reproducible in a unit test.

#include <cstdint>
#include <string>
#include <vector>

namespace mark4
{
    /// Which of the two decoded streams a packet belongs to.
    enum class StreamKind : std::uint8_t
    {
        TELEMETRY, ///< estimated state published by a flight process
        SIM_RAW    ///< exact state published by the simulator plant
    };

    /// @brief Name of a stream kind, as the status message spells it.
    /// @param stream stream to name
    /// @return static name
    const char *streamKindName(StreamKind stream);

    /// Counters of one (stream, source) link.
    struct LinkHealth
    {
        StreamKind stream = StreamKind::TELEMETRY; ///< stream the link carries
        std::uint8_t sourceId = 0U;                ///< StreamSource of the sender
        std::string sourceName;                    ///< name discovery gives that source,
                                                   ///< empty while nobody has announced it
        std::uint64_t received = 0U;               ///< packets seen, duplicates included
        std::uint64_t lost = 0U;                   ///< packets the numbering says never arrived
        std::uint64_t duplicates = 0U;             ///< packets carrying the previous number
        std::uint64_t resyncs = 0U;                ///< jumps too large to be a loss
        std::uint16_t lastSequence = 0U;           ///< number of the last packet seen
    };

    /// Sequence bookkeeping of every link the hub has seen since it started.
    class StreamHealth
    {
      public:
        /// A forward jump larger than this is a sender that restarted or a
        /// stream the hub only just joined, not a burst of losses: counting
        /// it as thousands of lost packets would drown the real ones.
        static constexpr std::uint16_t RESYNC_THRESHOLD = 1024U;

        /// @brief Accounts for one stream packet.
        /// @param stream stream it belongs to
        /// @param sourceId StreamSource of the sender
        /// @param sequence number the packet carries
        void onPacket(StreamKind stream, std::uint8_t sourceId, std::uint16_t sequence);

        /// @return every link seen so far, in the order they first appeared
        [[nodiscard]] const std::vector<LinkHealth> &links() const
        {
            return m_links;
        }

        /// @return the same links, writable, so a caller can name the sources
        ///         it knows about before publishing them
        [[nodiscard]] std::vector<LinkHealth> &links()
        {
            return m_links;
        }

      private:
        std::vector<LinkHealth> m_links; ///< one entry per (stream, source)
    };

    /// @brief Share of the packets a link should have carried that never
    ///        arrived.
    /// @param link link to score
    /// @return the ratio in [0, 1], 0 when nothing was expected yet
    double linkLossRate(const LinkHealth &link);
} // namespace mark4
