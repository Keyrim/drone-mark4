#pragma once

/// @file
/// @brief What a ground node keeps of the Status stream: one subscription
///        and one last report per drone, dropped when the node goes down.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "discovery/discovery_directory.hpp"
#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "mark4.pb.h"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    class StatusConsumerBase;

    /// Told what the Status consumer learns. Attaches to the consumer in its
    /// constructor and detaches in its destructor: declaring one as a member
    /// after the consumer is the whole wiring. Both bodies are inline like
    /// every other abstract class of the components: the library is built
    /// without RTTI and an out-of-line destructor would leave the typeinfo
    /// the RTTI-enabled executables reference undefined.
    class AbsStatusConsumerListener
    {
      public:
        /// @param consumer consumer to listen to; must outlive the listener
        explicit AbsStatusConsumerListener(StatusConsumerBase &consumer);

        virtual ~AbsStatusConsumerListener();

        AbsStatusConsumerListener(const AbsStatusConsumerListener &) = delete;
        AbsStatusConsumerListener &operator=(const AbsStatusConsumerListener &) = delete;

        /// @brief One report of a node the consumer follows.
        /// @param nodeId node it came from
        /// @param status the report
        /// @param nowUs instant of the poll that delivered it [us]
        virtual void onStatus(std::uint32_t nodeId,
                              const mark4_Status &status,
                              std::uint64_t nowUs) = 0;

        /// @brief A node went down: everything the consumer held of it is
        ///        gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        StatusConsumerBase &m_consumer; ///< where this listener is attached
    };

    /// The Status stream as a ground node holds it: it opens one entry per
    /// drone whose identity the directory learns, subscribes to its stream,
    /// and keeps the last report. The storage is the derived template's; this
    /// holds a span over it, so every composition sizes the table for itself
    /// without a second class of behaviour.
    class StatusConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
    {
      public:
        /// Body tags this handler consumes: the stream and the answer to the
        /// subscribe.
        static constexpr std::array<pb_size_t, 2> TAGS = {mark4_Envelope_status_tag,
                                                          mark4_Envelope_status_subscribe_tag};

        /// Node kinds that carry a StatusProvider. Nothing is asked of any
        /// other kind: it would acknowledge the request and drop it.
        static constexpr std::array<mark4_NodeKind, 2> KINDS = {mark4_NodeKind_FIRMWARE,
                                                                mark4_NodeKind_DRONE_SIM};

        /// Listeners one consumer may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 2U;

        /// What the consumer holds of one node.
        struct Entry
        {
            std::uint32_t id = 0U;               ///< node id
            bool subscribed = false;             ///< the node took the subscribe
            std::uint32_t subscribeRequest = 0U; ///< subscribe waiting for its answer, 0 none
            bool hasStatus = false;              ///< a report arrived
            mark4_Status last = mark4_Status_init_zero; ///< the last one, valid when hasStatus
            std::uint64_t receivedUs = 0U;              ///< instant it arrived [us]
            std::uint32_t received = 0U;                ///< reports from this node
        };

        /// @param messenger messenger the stream comes from and the
        ///        subscribes leave by; must outlive the consumer
        /// @param directory directory whose identities open the entries;
        ///        must outlive the consumer
        /// @param storage the entry table, owned by the derived template
        StatusConsumerBase(Messenger &messenger,
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

        /// @brief A node said what it is: a drone speaking this schema is
        ///        opened and subscribed to. A node already open is left
        ///        alone, so an announce that only changed a name does not
        ///        restart anything.
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

        /// @brief One report, or the answer to a subscribe.
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
            if (envelope.which_body == mark4_Envelope_status_subscribe_tag)
            {
                entry->subscribed = envelope.body.status_subscribe.enabled;
                entry->subscribeRequest = 0U;
                return true;
            }
            if (envelope.which_body != mark4_Envelope_status_tag)
            {
                return false;
            }
            entry->hasStatus = true;
            entry->last = envelope.body.status;
            entry->receivedUs = nowUs;
            ++entry->received;
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onStatus(src, envelope.body.status, nowUs);
            }
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
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onForgotten(nodeId);
            }
        }

        /// @brief The subscribe was never acknowledged: the node is present
        ///        and deaf to it, and the stream will not come.
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
            Module().warn("%08lx never took the status subscribe", static_cast<unsigned long>(dst));
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

        /// @return requests started, the refused ones included
        [[nodiscard]] std::uint32_t requests() const
        {
            return m_requests;
        }

        /// @return nodes that appeared while the table was full
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        friend class AbsStatusConsumerListener;

        /// @return the logging module of the consumer. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_STATUS_CONSUMER, "status/consumer"};
            return MODULE;
        }

        /// @brief Opens one node and subscribes to its stream. A node
        ///        already open is left alone.
        /// @param id node to open
        void open(std::uint32_t id)
        {
            if (lookup(id) != nullptr)
            {
                return;
            }
            if (m_count == m_entries.size())
            {
                ++m_dropped;
                Module().warn("no room for %08lx: %zu node(s) already",
                              static_cast<unsigned long>(id),
                              m_count);
                return;
            }
            Entry &entry = m_entries[m_count];
            ++m_count;
            entry = Entry{};
            entry.id = id;
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_status_subscribe_tag;
            ask.body.status_subscribe.enabled = true;
            entry.subscribeRequest = request(id, ask);
            ++m_requests;
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

        /// @brief Adds one listener. Past MAX_LISTENERS it is not added and
        ///        init() fails for as long as it lives.
        /// @param listener listener to add
        void attach(AbsStatusConsumerListener &listener)
        {
            if (m_listenerCount >= MAX_LISTENERS)
            {
                ++m_listenersOverflow;
                return;
            }
            m_listeners[m_listenerCount] = &listener;
            ++m_listenerCount;
        }

        /// @brief Removes one listener, if present.
        /// @param listener listener to remove
        void detach(AbsStatusConsumerListener &listener)
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

        std::span<Entry> m_entries; ///< table, not owned
        std::size_t m_count = 0U;   ///< entries in use
        std::array<AbsStatusConsumerListener *, MAX_LISTENERS>
            m_listeners{};                      ///< attached, in order
        std::size_t m_listenerCount = 0U;       ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        std::uint32_t m_requests = 0U;          ///< requests started
        std::uint32_t m_dropped = 0U;           ///< nodes that found the table full
    };

    /// The Status consumer with its storage: N nodes followed at once, a
    /// constant of the composition.
    ///
    /// @tparam N nodes the consumer follows at most
    template <std::size_t N> class StatusConsumer final : public StatusConsumerBase
    {
      public:
        /// @param messenger messenger to attach to
        /// @param directory directory to listen to
        StatusConsumer(Messenger &messenger, DiscoveryDirectory &directory)
            : StatusConsumerBase(messenger, directory, m_storage)
        {
        }

      private:
        std::array<Entry, N> m_storage{}; ///< the entries the base spans
    };

    inline AbsStatusConsumerListener::AbsStatusConsumerListener(StatusConsumerBase &consumer)
        : m_consumer(consumer)
    {
        m_consumer.attach(*this);
    }

    inline AbsStatusConsumerListener::~AbsStatusConsumerListener()
    {
        m_consumer.detach(*this);
    }
} // namespace mark4
