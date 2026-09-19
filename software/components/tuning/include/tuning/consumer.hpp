#pragma once

/// @file
/// @brief What a ground node keeps of another node's parameter table:
///        the table itself, pulled page by page, and the answer to every
///        set and get it sent.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <pb.h>

#include "discovery/discovery_directory.hpp"
#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "messaging/table_pull.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    class TuningConsumerBase;

    /// Told what the tuning consumer learns. Attaches to the consumer in its
    /// constructor and detaches in its destructor: declaring one as a member
    /// after the consumer is the whole wiring. Both bodies are inline like
    /// every other abstract class of the components: the library is built
    /// without RTTI and an out-of-line destructor would leave the typeinfo
    /// the RTTI-enabled executables reference undefined.
    class AbsTuningConsumerListener
    {
      public:
        /// @param consumer consumer to listen to; must outlive the listener
        explicit AbsTuningConsumerListener(TuningConsumerBase &consumer);

        virtual ~AbsTuningConsumerListener();

        AbsTuningConsumerListener(const AbsTuningConsumerListener &) = delete;
        AbsTuningConsumerListener &operator=(const AbsTuningConsumerListener &) = delete;

        /// @brief One node's parameter table is whole.
        /// @param nodeId node the table belongs to
        /// @param infos the table, in table order
        virtual void onTable(std::uint32_t nodeId, std::span<const mark4_TuningInfo> infos) = 0;

        /// @brief The answer to one set or one get.
        /// @param nodeId node that answered
        /// @param ack the parameter, the value in effect and the outcome
        virtual void onResult(std::uint32_t nodeId, const mark4_TuningAck &ack) = 0;

        /// @brief A node went down: everything the consumer held of it is
        ///        gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        TuningConsumerBase &m_consumer; ///< where this listener is attached
    };

    /// Another node's parameters as a ground node holds them: it opens one
    /// entry per drone whose identity the directory learns and walks its
    /// parameter table one page at a time. The storage is the derived
    /// template's; this holds a span over it, so every composition sizes the
    /// table for itself without a second class of behaviour.
    class TuningConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
    {
      public:
        /// Body tags this handler consumes: the table pages and the answers.
        static constexpr std::array<pb_size_t, 2> TAGS = {mark4_Envelope_tuning_ack_tag,
                                                          mark4_Envelope_tuning_infos_tag};

        /// Node kinds that carry a TuningProvider. Nothing is asked of any
        /// other kind: it would acknowledge the request and drop it.
        static constexpr std::array<mark4_NodeKind, 2> KINDS = {mark4_NodeKind_FIRMWARE,
                                                                mark4_NodeKind_DRONE_SIM};

        /// Parameters kept per node: the profile bound of the ground side.
        static constexpr std::size_t MAX_PARAMS = 64U;

        /// Listeners one consumer may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 2U;

        /// What the consumer holds of one node.
        struct Entry
        {
            std::uint32_t id = 0U;                         ///< node id
            TablePull<mark4_TuningInfo, MAX_PARAMS> table; ///< the parameter table walk
        };

        /// @param messenger messenger the pages and answers come from and
        ///        the requests leave by; must outlive the consumer
        /// @param directory directory whose identities open the entries;
        ///        must outlive the consumer
        /// @param storage the entry table, owned by the derived template
        TuningConsumerBase(Messenger &messenger,
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

        /// @brief Pulls one node's parameter table again, from the first
        ///        page.
        /// @param id node to ask
        /// @return true when the page request was taken
        bool refresh(std::uint32_t id)
        {
            Entry *const entry = lookup(id);
            if (entry == nullptr)
            {
                return false;
            }
            entry->table.reset();
            return askPage(*entry);
        }

        /// @brief Writes one parameter of one node. The answer carries the
        ///        value in effect, whatever the outcome.
        /// @param id node to ask
        /// @param paramId parameter to write
        /// @param value value to write
        /// @return true when the request was taken
        bool set(std::uint32_t id, std::uint32_t paramId, float value)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_tuning_set_tag;
            ask.body.tuning_set.id = paramId;
            ask.body.tuning_set.value = value;
            ++m_requests;
            return request(id, ask) != 0U;
        }

        /// @brief Reads one parameter of one node.
        /// @param id node to ask
        /// @param paramId parameter to read
        /// @return true when the request was taken
        bool get(std::uint32_t id, std::uint32_t paramId)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_tuning_get_tag;
            ask.body.tuning_get.id = paramId;
            ++m_requests;
            return request(id, ask) != 0U;
        }

        /// @brief A node said what it is: a drone speaking this schema is
        ///        opened and its table pulled. A node already open is left
        ///        alone, so an announce that only changed a name does not
        ///        restart the walk.
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

        /// @brief One table page, or the answer to a set or a get.
        /// @param src node it came from
        /// @param envelope the message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        nothing here is timed
        /// @return true when the message was acted on
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            static_cast<void>(nowUs);
            Entry *const entry = lookup(src);
            if (entry == nullptr)
            {
                // Nothing was asked of that node: what it says is not this
                // consumer's business.
                return false;
            }
            if (envelope.which_body == mark4_Envelope_tuning_infos_tag)
            {
                onPage(*entry, envelope.body.tuning_infos);
                return true;
            }
            if (envelope.which_body != mark4_Envelope_tuning_ack_tag)
            {
                return false;
            }
            onAck(*entry, envelope.body.tuning_ack);
            return true;
        }

        /// @brief A node went down: its entry goes and the listeners are
        ///        told.
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

        /// @brief A page request was never acknowledged: the walk stops
        ///        where it got to.
        /// @param dst node it went to
        /// @param requestId id it carried
        void onRequestFailed(std::uint32_t dst, std::uint32_t requestId) override
        {
            Entry *const entry = lookup(dst);
            if (entry == nullptr || entry->table.requestId() != requestId)
            {
                return;
            }
            entry->table.abandon();
            Module().warn("%08lx never answered for its parameter table",
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
        friend class AbsTuningConsumerListener;

        /// @return the logging module of the consumer. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TUNING_CONSUMER, "tuning/consumer"};
            return MODULE;
        }

        /// @brief Opens one node and starts its table. A node already open
        ///        is left alone.
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
            static_cast<void>(askPage(entry));
        }

        /// @brief Asks one node for the page its walk waits on.
        /// @param entry node to ask
        /// @return true when the request was taken
        bool askPage(Entry &entry)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_tuning_list_request_tag;
            ask.body.tuning_list_request.cursor = entry.table.cursor();
            ++m_requests;
            entry.table.setRequestId(request(entry.id, ask));
            return entry.table.requestId() != 0U;
        }

        /// @brief Merges one page and asks for the next, or publishes the
        ///        table when it is whole.
        /// @param entry node the page came from
        /// @param page the page
        void onPage(Entry &entry, const mark4_TuningInfos &page)
        {
            if (!entry.table.applyPage(
                    page.total,
                    page.cursor,
                    std::span<const mark4_TuningInfo>(page.infos, page.infos_count)))
            {
                return; // a duplicate or a stale answer
            }
            if (!entry.table.complete())
            {
                static_cast<void>(askPage(entry));
                return;
            }
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onTable(entry.id, entry.table.items());
            }
        }

        /// @brief Takes one answer: a write that went through moves the
        ///        value in the table, so what the consumer holds is what
        ///        flies.
        /// @param entry node that answered
        /// @param ack the answer
        void onAck(Entry &entry, const mark4_TuningAck &ack)
        {
            if (ack.status == mark4_TuningStatus_OK && entry.table.complete())
            {
                for (mark4_TuningInfo &known : entry.table.items())
                {
                    if (known.id == ack.id)
                    {
                        known.value = ack.value;
                        break;
                    }
                }
            }
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onResult(entry.id, ack);
            }
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
        void attach(AbsTuningConsumerListener &listener)
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
        void detach(AbsTuningConsumerListener &listener)
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
        std::array<AbsTuningConsumerListener *, MAX_LISTENERS>
            m_listeners{};                      ///< attached, in order
        std::size_t m_listenerCount = 0U;       ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        std::uint32_t m_requests = 0U;          ///< requests started
        std::uint32_t m_dropped = 0U;           ///< nodes that found the table full
    };

    /// The tuning consumer with its storage: N nodes followed at once, a
    /// constant of the composition.
    ///
    /// @tparam N nodes the consumer follows at most
    template <std::size_t N> class TuningConsumer final : public TuningConsumerBase
    {
      public:
        /// @param messenger messenger to attach to
        /// @param directory directory to listen to
        TuningConsumer(Messenger &messenger, DiscoveryDirectory &directory)
            : TuningConsumerBase(messenger, directory, m_storage)
        {
        }

      private:
        std::array<Entry, N> m_storage{}; ///< the entries the base spans
    };

    inline AbsTuningConsumerListener::AbsTuningConsumerListener(TuningConsumerBase &consumer)
        : m_consumer(consumer)
    {
        m_consumer.attach(*this);
    }

    inline AbsTuningConsumerListener::~AbsTuningConsumerListener()
    {
        m_consumer.detach(*this);
    }
} // namespace mark4
