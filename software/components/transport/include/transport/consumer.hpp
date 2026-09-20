#pragma once

/// @file
/// @brief What a ground node keeps of the transport reports: one
///        subscription and one last report per node, its pages merged into
///        the peer table the reporting node holds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "discovery/discovery_directory.hpp"
#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    class AbsTransportConsumerListener;

    /// The transport reports as a ground node holds them: it opens one entry
    /// per node whose identity the directory learns, subscribes to its
    /// reports while they are wanted, and merges the pages of each report
    /// into one view. The storage is the derived template's; this holds a
    /// span over it, so every composition sizes the table for itself without
    /// a second class of behaviour.
    class TransportConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
    {
      public:
        /// Body tags this handler consumes: the stream and the subscription
        /// the node holds. Never the request tag: the provider of the same
        /// concept claims that one, and both may live on one node.
        static constexpr std::array<pb_size_t, 2> TAGS = {
            mark4_Envelope_transport_report_tag, mark4_Envelope_transport_subscription_tag};

        /// Node kinds that carry a TransportProvider. Nothing is asked of any
        /// other kind: it would acknowledge the request and drop it.
        static constexpr std::array<mark4_NodeKind, 4> KINDS = {mark4_NodeKind_FIRMWARE,
                                                                mark4_NodeKind_DRONE_SIM,
                                                                mark4_NodeKind_RELAY,
                                                                mark4_NodeKind_GATEWAY};

        /// Listeners one consumer may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 2U;

        /// Peers one report may hold once its pages are merged: a
        /// transport's table.
        static constexpr std::size_t MAX_PEERS = Transport::MAX_NODES;

        /// What the consumer holds of one node.
        struct Entry
        {
            std::uint32_t id = 0U;               ///< node id
            bool local = false;                  ///< this node's own report, fed by accept(),
                                                 ///< never subscribed
            bool subscribed = false;             ///< the node took the subscribe
            std::uint32_t subscribeRequest = 0U; ///< subscribe (or unsubscribe) waiting for its
                                                 ///< answer, 0 none
            bool hasReport = false;              ///< a complete report arrived
            /// counters and links of the last complete report; its peers
            /// field is unused, the table below holds them
            mark4_TransportReport last = mark4_TransportReport_init_zero;
            std::array<mark4_TransportPeer, MAX_PEERS>
                peers{};                   ///< merged peer table of that report
            std::size_t peerCount = 0U;    ///< peers in peers
            std::uint64_t receivedUs = 0U; ///< instant the last complete report arrived [us]
            std::uint32_t reports = 0U;    ///< complete reports from this node
            std::array<mark4_TransportPeer, MAX_PEERS> staging{}; ///< report being received
            std::size_t stagingCount = 0U;                        ///< peers in staging
            bool stagingValid = false; ///< a page with cursor 0 opened the staging
        };

        /// @param messenger messenger the reports come from and the
        ///        subscribes leave by; must outlive the consumer
        /// @param directory directory whose identities open the entries;
        ///        must outlive the consumer
        /// @param storage the entry table, owned by the derived template
        TransportConsumerBase(Messenger &messenger,
                              DiscoveryDirectory &directory,
                              std::span<Entry> storage)
            : AbsMessageHandler(messenger, TAGS),
              AbsDirectoryListener(directory),
              m_entries(storage)
        {
        }

        /// @brief Checks the composition: at most MAX_LISTENERS listeners.
        /// @return true when every listener that attached is heard
        [[nodiscard]] bool init() const
        {
            return m_listenersOverflow == 0U;
        }

        /// @brief Asks every node open now and later for its reports, or
        ///        stops asking: one TransportSubscribe per remote entry, as
        ///        a request. Turning it off also forgets the reports held of
        ///        the remote entries, so nothing stale is published; the
        ///        local entry keeps its own.
        /// @param wanted true to hold every node's reports
        void setWanted(bool wanted)
        {
            if (m_wanted == wanted)
            {
                return;
            }
            m_wanted = wanted;
            for (std::size_t index = 0U; index < m_count; ++index)
            {
                Entry &entry = m_entries[index];
                if (entry.local)
                {
                    continue;
                }
                if (wanted)
                {
                    subscribe(entry, true);
                    continue;
                }
                if (entry.subscribed)
                {
                    subscribe(entry, false);
                }
                entry.hasReport = false;
                entry.peerCount = 0U;
                entry.stagingCount = 0U;
                entry.stagingValid = false;
            }
        }

        /// @return true while the reports of every node are wanted
        [[nodiscard]] bool wanted() const
        {
            return m_wanted;
        }

        /// @brief Opens the entry of this node's own report, fed by accept()
        ///        and never subscribed to. Once, by the composition.
        /// @param id this node's own id
        /// @return false when the table is full
        bool openLocal(std::uint32_t id)
        {
            Entry *const existing = lookup(id);
            if (existing != nullptr)
            {
                existing->local = true;
                return true;
            }
            if (m_count == m_entries.size())
            {
                Module().warn("no room for the local entry %08lx: %zu node(s) already",
                              static_cast<unsigned long>(id),
                              m_count);
                return false;
            }
            Entry &entry = m_entries[m_count];
            ++m_count;
            entry = Entry{};
            entry.id = id;
            entry.local = true;
            return true;
        }

        /// @brief Takes one page of one node's report: the counters are
        ///        copied, the peer slice the page names replaces the
        ///        staging, and the last page of a report makes the staging
        ///        the report the listeners hear. A page with cursor 0 always
        ///        opens a new staging; a page whose cursor is not where the
        ///        staging ended is dropped, so a report that lost a page is
        ///        abandoned and the next one starts over.
        /// @param nodeId node the page came from
        /// @param page the page
        /// @param nowUs instant it arrived [us]
        void accept(std::uint32_t nodeId, const mark4_TransportReport &page, std::uint64_t nowUs)
        {
            Entry *const entry = lookup(nodeId);
            if (entry == nullptr)
            {
                return;
            }
            if (page.peer_cursor == 0U)
            {
                entry->stagingCount = 0U;
                entry->stagingValid = true;
            }
            else if (!entry->stagingValid ||
                     page.peer_cursor != static_cast<std::uint32_t>(entry->stagingCount))
            {
                entry->stagingValid = false;
                return;
            }
            for (pb_size_t index = 0U; index < page.peers_count && entry->stagingCount < MAX_PEERS;
                 ++index)
            {
                entry->staging[entry->stagingCount] = page.peers[index];
                ++entry->stagingCount;
            }
            if (page.peer_cursor + page.peers_count < page.peer_total)
            {
                return; // more pages to come
            }
            entry->last = page;
            // The peers of the last page alone would say less than nothing:
            // the merged table below is what the report holds.
            entry->last.peers_count = 0U;
            entry->peers = entry->staging;
            entry->peerCount = entry->stagingCount;
            entry->hasReport = true;
            entry->receivedUs = nowUs;
            ++entry->reports;
            entry->stagingValid = false;
            notifyReport(nodeId, *entry, nowUs);
        }

        /// @brief A node said what it is: a node carrying a provider of this
        ///        concept and speaking this schema is opened, and subscribed
        ///        to while the reports are wanted. A node already open is
        ///        left alone.
        /// @param entry the directory entry, as it stands
        void onIdentity(const DirectoryEntry &entry) override
        {
            if (entry.wireMismatch)
            {
                return;
            }
            for (const mark4_NodeKind kind : KINDS)
            {
                if (entry.announce.kind == kind)
                {
                    open(entry.id);
                    return;
                }
            }
        }

        /// @brief The directory forgot a node. Nothing to do: presence does
        ///        it, through onNodeDown().
        /// @param nodeId the node
        void onForgotten(std::uint32_t nodeId) override
        {
            static_cast<void>(nodeId);
        }

        /// @brief One page of a report, or the subscription the node holds.
        /// @param src node it came from
        /// @param envelope the message
        /// @param nowUs instant of the poll that delivered it [us]
        /// @return true when the message was acted on
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            Entry *const entry = lookup(src);
            if (entry == nullptr)
            {
                // Nothing was asked of that node: its report is not this
                // consumer's business.
                return false;
            }
            if (envelope.which_body == mark4_Envelope_transport_subscription_tag)
            {
                entry->subscribed = envelope.body.transport_subscription.enabled;
                entry->subscribeRequest = 0U;
                return true;
            }
            if (envelope.which_body != mark4_Envelope_transport_report_tag)
            {
                return false;
            }
            accept(src, envelope.body.transport_report, nowUs);
            return true;
        }

        /// @brief A node went down: its entry goes and the listeners are
        ///        told. A reincarnation comes back as a node up and a fresh
        ///        identity, which opens it again.
        /// @param nodeId the node
        void onNodeDown(std::uint32_t nodeId) override
        {
            Entry *const entry = lookup(nodeId);
            if (entry == nullptr)
            {
                return;
            }
            // The order of the entries carries no meaning, so the last one
            // takes the freed slot rather than shifting the whole prefix.
            const std::size_t last = m_count - 1U;
            *entry = m_entries[last];
            m_entries[last] = Entry{};
            m_count = last;
            notifyForgotten(nodeId);
        }

        /// @brief The subscribe was never acknowledged: the node is present
        ///        and deaf to it, and the reports will not come.
        /// @param dst node it went to
        /// @param requestId id it carried
        void onRequestFailed(std::uint32_t dst, std::uint32_t requestId) override
        {
            Entry *const entry = lookup(dst);
            if (entry == nullptr || entry->subscribeRequest != requestId)
            {
                return;
            }
            entry->subscribeRequest = 0U;
            Module().warn("%08lx never took the transport subscribe",
                          static_cast<unsigned long>(dst));
        }

        /// @param id node to look up
        /// @return its entry, nullptr when the consumer holds none
        [[nodiscard]] const Entry *find(std::uint32_t id) const
        {
            for (std::size_t index = 0U; index < m_count; ++index)
            {
                if (m_entries[index].id == id)
                {
                    return &m_entries[index];
                }
            }
            return nullptr;
        }

        /// @return nodes held
        [[nodiscard]] std::size_t size() const
        {
            return m_count;
        }

        /// @param index 0 <= index < size()
        /// @return one entry; the entries form a dense prefix whose order
        ///         carries no meaning
        [[nodiscard]] const Entry &entry(std::size_t index) const
        {
            return m_entries[index];
        }

      private:
        friend class AbsTransportConsumerListener;

        /// @return the logging module of the consumer. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TRANSPORT_CONSUMER, "transport/consumer"};
            return MODULE;
        }

        /// @brief Opens one remote node, and subscribes to it when the
        ///        reports are wanted. A node already open is left alone.
        /// @param id node to open
        void open(std::uint32_t id)
        {
            if (lookup(id) != nullptr)
            {
                return;
            }
            if (m_count == m_entries.size())
            {
                Module().warn("no room for %08lx: %zu node(s) already",
                              static_cast<unsigned long>(id),
                              m_count);
                return;
            }
            Entry &entry = m_entries[m_count];
            ++m_count;
            entry = Entry{};
            entry.id = id;
            if (m_wanted)
            {
                subscribe(entry, true);
            }
        }

        /// @brief Asks one node to start or stop its reports.
        /// @param entry the node's entry
        /// @param enabled true to hold its reports
        void subscribe(Entry &entry, bool enabled)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_transport_subscribe_tag;
            ask.body.transport_subscribe.enabled = enabled;
            entry.subscribeRequest = request(entry.id, ask);
        }

        /// @param id node to look up
        /// @return mutable entry, nullptr when unknown
        Entry *lookup(std::uint32_t id)
        {
            for (std::size_t index = 0U; index < m_count; ++index)
            {
                if (m_entries[index].id == id)
                {
                    return &m_entries[index];
                }
            }
            return nullptr;
        }

        /// @brief Tells every listener one node's report is complete.
        /// @param nodeId node it came from
        /// @param entry the entry, as it stands
        /// @param nowUs instant the last page arrived [us]
        void notifyReport(std::uint32_t nodeId, const Entry &entry, std::uint64_t nowUs);

        /// @brief Tells every listener one node is gone.
        /// @param nodeId the node
        void notifyForgotten(std::uint32_t nodeId);

        /// @brief Adds one listener. Past MAX_LISTENERS it is not added and
        ///        init() fails for as long as it lives.
        /// @param listener listener to add
        void attach(AbsTransportConsumerListener &listener);

        /// @brief Removes one listener, if present.
        /// @param listener listener to remove
        void detach(AbsTransportConsumerListener &listener);

        std::span<Entry> m_entries; ///< table, not owned
        std::size_t m_count = 0U;   ///< entries in use
        std::array<AbsTransportConsumerListener *, MAX_LISTENERS>
            m_listeners{};                      ///< attached, in order
        std::size_t m_listenerCount = 0U;       ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        bool m_wanted = false;                  ///< the reports of every node are wanted
    };

    /// Told what the transport consumer learns. Attaches to the consumer in
    /// its constructor and detaches in its destructor: declaring one as a
    /// member after the consumer is the whole wiring. Both bodies are inline
    /// like every other abstract class of the components: the library is
    /// built without RTTI and an out-of-line destructor would leave the
    /// typeinfo the RTTI-enabled executables reference undefined.
    class AbsTransportConsumerListener
    {
      public:
        /// @param consumer consumer to listen to; must outlive the listener
        explicit AbsTransportConsumerListener(TransportConsumerBase &consumer)
            : m_consumer(consumer)
        {
            m_consumer.attach(*this);
        }

        virtual ~AbsTransportConsumerListener()
        {
            m_consumer.detach(*this);
        }

        AbsTransportConsumerListener(const AbsTransportConsumerListener &) = delete;
        AbsTransportConsumerListener &operator=(const AbsTransportConsumerListener &) = delete;

        /// @brief One node's report, pages merged: fired when the last page
        ///        of a report arrived.
        /// @param nodeId node it came from
        /// @param entry what the consumer holds of that node
        /// @param nowUs instant the last page arrived [us]
        virtual void onReport(std::uint32_t nodeId,
                              const TransportConsumerBase::Entry &entry,
                              std::uint64_t nowUs) = 0;

        /// @brief A node went down: everything the consumer held of it is
        ///        gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        TransportConsumerBase &m_consumer; ///< where this listener is attached
    };

    /// The transport consumer with its storage: N nodes followed at once, a
    /// constant of the composition.
    ///
    /// @tparam N nodes the consumer follows at most
    template <std::size_t N> class TransportConsumer final : public TransportConsumerBase
    {
      public:
        /// @param messenger messenger to attach to
        /// @param directory directory to listen to
        TransportConsumer(Messenger &messenger, DiscoveryDirectory &directory)
            : TransportConsumerBase(messenger, directory, m_storage)
        {
        }

      private:
        std::array<Entry, N> m_storage{}; ///< the entries the base spans
    };

    inline void TransportConsumerBase::notifyReport(std::uint32_t nodeId,
                                                    const Entry &entry,
                                                    std::uint64_t nowUs)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            m_listeners[index]->onReport(nodeId, entry, nowUs);
        }
    }

    inline void TransportConsumerBase::notifyForgotten(std::uint32_t nodeId)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            m_listeners[index]->onForgotten(nodeId);
        }
    }

    inline void TransportConsumerBase::attach(AbsTransportConsumerListener &listener)
    {
        if (m_listenerCount >= MAX_LISTENERS)
        {
            ++m_listenersOverflow;
            return;
        }
        m_listeners[m_listenerCount] = &listener;
        ++m_listenerCount;
    }

    inline void TransportConsumerBase::detach(AbsTransportConsumerListener &listener)
    {
        for (std::size_t index = 0U; index < m_listenerCount; ++index)
        {
            if (m_listeners[index] == &listener)
            {
                --m_listenerCount;
                for (std::size_t next = index; next < m_listenerCount; ++next)
                {
                    m_listeners[next] = m_listeners[next + 1U];
                }
                m_listeners[m_listenerCount] = nullptr;
                return;
            }
        }
        if (m_listenersOverflow > 0U)
        {
            --m_listenersOverflow;
        }
    }
} // namespace mark4
