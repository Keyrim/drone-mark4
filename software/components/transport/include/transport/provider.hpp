#pragma once

/// @file
/// @brief One node's view of the transport on the wire: the subscribe
///        request in, the report out to every node that asked for it, by
///        pages of peers.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "messaging/subscriber_table.hpp"
#include "protocol/envelope.hpp"
#include "transport/link.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// What this node's transport and messenger count, streamed to every
    /// node that subscribed. The counters travel cumulative and never as
    /// rates, so a lost report skews nothing and a consumer takes the
    /// differences itself. The peer table does not fit one frame, so it
    /// travels by pages: every page carries the counters again and names the
    /// slice of the table it holds. With no subscriber nothing is packed at
    /// all.
    class TransportProvider final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 1> TAGS = {mark4_Envelope_transport_subscribe_tag};

        /// Silence between two reports [us].
        static constexpr std::uint64_t REPORT_PERIOD_US = 1'000'000U;

        /// Nodes that may hold the stream at once. Two: a board's UART pays
        /// every entry, next to the status stream and the log lines.
        static constexpr std::size_t MAX_SUBSCRIBERS = 2U;

        /// Peers one page carries: the bound of the repeated field.
        static constexpr std::size_t PEERS_PER_PAGE = 4U;

        /// @param messenger the subscribes come from it, the reports leave
        ///        by it, and its counters are part of the report; must
        ///        outlive the provider
        /// @param transport what is reported; must outlive the provider
        TransportProvider(Messenger &messenger, const Transport &transport)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger),
              m_transport(transport)
        {
        }

        /// @brief Sends the report when due, to every subscriber: one page
        ///        per PEERS_PER_PAGE peers, all pages in the same call. With
        ///        no subscriber nothing is packed at all.
        /// @param nowUs current instant [us], from the caller's clock
        void tick(std::uint64_t nowUs)
        {
            if (m_subscribers.empty())
            {
                return;
            }
            if (m_reported && nowUs - m_lastReportUs < REPORT_PERIOD_US)
            {
                return;
            }
            m_lastReportUs = nowUs;
            m_reported = true;

            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_transport_report_tag;
            mark4_TransportReport &page = envelope.body.transport_report;
            std::uint32_t cursor = 0U;
            do
            {
                const std::size_t written = fillPage(cursor, nowUs, page);
                for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
                {
                    // Stream data: sent once, never resent. The counters are
                    // cumulative, so the next report carries what this one
                    // would have said.
                    if (m_messenger.send(m_subscribers.id(index), envelope))
                    {
                        ++m_pageCount;
                    }
                }
                cursor += static_cast<std::uint32_t>(written);
            } while (cursor < page.peer_total);
        }

        /// @brief Fills one page of the report, as tick() sends it. Public
        ///        so a node that consumes its own report can walk the pages
        ///        without the wire.
        /// @param cursor index of the first peer of the page
        /// @param nowUs current instant [us], for the ages
        /// @param[out] out the page
        /// @return peers written, 0 when cursor is past the table (the page
        ///         still carries the counters and peer_total)
        std::size_t fillPage(std::uint32_t cursor,
                             std::uint64_t nowUs,
                             mark4_TransportReport &out) const
        {
            out = mark4_TransportReport_init_zero;
            out.sent = m_transport.sent();
            out.sent_bytes = static_cast<std::uint32_t>(m_transport.sentBytes());
            out.refused = m_transport.refused();
            out.dropped = m_transport.dropped();
            out.relayed = m_transport.relayed();
            out.expired = m_transport.expired();
            out.restarted = m_transport.restarted();
            out.undecodable = m_messenger.undecodable();
            out.unhandled = m_messenger.unhandled();
            out.requests = m_messenger.requests();
            out.resent = m_messenger.resent();
            out.completed = m_messenger.completed();
            out.failed = m_messenger.failed();
            out.unmatched_acks = m_messenger.unmatchedAcks();

            const std::size_t links = std::min(m_transport.linkCount(), MAX_WIRE_LINKS);
            out.links_count = static_cast<pb_size_t>(links);
            for (std::size_t index = 0U; index < links; ++index)
            {
                const AbsLink &link = m_transport.link(index);
                const Transport::LinkStats &stats = m_transport.linkStats(index);
                mark4_TransportLink &wire = out.links[index];
                wire.kind = WireKind(link.kind());
                wire.frames_in = stats.framesIn;
                wire.bytes_in = stats.bytesIn;
                wire.frames_out = stats.framesOut;
                wire.bytes_out = stats.bytesOut;
                wire.refused = stats.refused;
                wire.rx_errors = link.rxErrors();
            }

            out.peer_cursor = cursor;
            out.peer_total = static_cast<std::uint32_t>(m_transport.nodeCount());
            std::size_t written = 0U;
            while (written < PEERS_PER_PAGE && cursor + written < out.peer_total)
            {
                const Transport::Node &node = m_transport.node(cursor + written);
                mark4_TransportPeer &peer = out.peers[written];
                peer.id = node.id;
                peer.link = static_cast<std::uint32_t>(node.link);
                peer.hops = node.hops;
                peer.received = node.received;
                peer.lost = node.lost;
                peer.duplicates = node.duplicates;
                // A frame stamped after this instant (a clock that went
                // backwards) reads as no age at all rather than as an
                // enormous one.
                peer.age_ms = static_cast<std::uint32_t>(
                    (nowUs - std::min(nowUs, node.lastSeenUs)) / US_PER_MS);
                ++written;
            }
            out.peers_count = static_cast<pb_size_t>(written);
            return written;
        }

        /// @brief Takes one subscribe request and answers it with the
        ///        subscription as it stands.
        /// @param src node it came from: the subscriber, and where the
        ///        answer goes
        /// @param envelope decoded message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        the stream is paced by tick()
        /// @return true when the message was answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            static_cast<void>(nowUs);
            if (envelope.which_body != mark4_Envelope_transport_subscribe_tag)
            {
                return false;
            }
            bool applied = false;
            if (envelope.body.transport_subscribe.enabled)
            {
                applied = m_subscribers.add(src);
                if (!applied)
                {
                    Module().warn("no room for %08lx: %zu subscribers already",
                                  static_cast<unsigned long>(src),
                                  m_subscribers.size());
                }
            }
            else
            {
                static_cast<void>(m_subscribers.remove(src));
            }
            mark4_Envelope answer = mark4_Envelope_init_zero;
            answer.which_body = mark4_Envelope_transport_subscription_tag;
            answer.body.transport_subscription.enabled = applied;
            static_cast<void>(request(src, answer));
            return true;
        }

        /// @brief A node went down: it is not listening any more.
        /// @param nodeId the node
        void onNodeDown(std::uint32_t nodeId) override
        {
            if (m_subscribers.remove(nodeId))
            {
                Module().info("%08lx gone, %zu subscriber(s) left",
                              static_cast<unsigned long>(nodeId),
                              m_subscribers.size());
            }
        }

        /// @return nodes holding the stream
        [[nodiscard]] std::size_t subscribers() const
        {
            return m_subscribers.size();
        }

        /// @return report pages sent since construction
        [[nodiscard]] std::uint32_t reportsSent() const
        {
            return m_pageCount;
        }

      private:
        /// Links one report carries at most: the bound of the repeated field.
        static constexpr std::size_t MAX_WIRE_LINKS = 4U;

        // The two page sizes are the nanopb bounds of mark4.options, spelled
        // here so a page never overruns the generated arrays.
        static_assert(PEERS_PER_PAGE == std::size(mark4_TransportReport{}.peers),
                      "PEERS_PER_PAGE is not the bound of TransportReport.peers");
        static_assert(MAX_WIRE_LINKS == std::size(mark4_TransportReport{}.links),
                      "MAX_WIRE_LINKS is not the bound of TransportReport.links");

        /// Microseconds in one millisecond, the unit the ages travel in.
        static constexpr std::uint64_t US_PER_MS = 1000U;

        /// @param kind medium of a link
        /// @return the same medium as the schema names it
        static mark4_LinkKind WireKind(LinkKind kind)
        {
            switch (kind)
            {
                case LinkKind::UART:
                    return mark4_LinkKind_LINK_UART;
                case LinkKind::UDP:
                    return mark4_LinkKind_LINK_UDP;
            }
            return mark4_LinkKind_LINK_KIND_UNSPECIFIED;
        }

        /// @return the logging module of the provider. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TRANSPORT_PROVIDER, "transport/provider"};
            return MODULE;
        }

        Messenger &m_messenger;                         ///< reports leave by it, not owned
        const Transport &m_transport;                   ///< what is reported, not owned
        SubscriberTable<MAX_SUBSCRIBERS> m_subscribers; ///< nodes holding the stream
        std::uint64_t m_lastReportUs = 0U;              ///< instant of the last report [us]
        bool m_reported = false;                        ///< true once one report went out
        std::uint32_t m_pageCount = 0U;                 ///< report pages sent
    };
} // namespace mark4
