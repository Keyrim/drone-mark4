#pragma once

/// @file
/// @brief What a ground node keeps of another node's log: its module table,
///        pulled page by page, and its line stream while it is subscribed.

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
#include "messaging/table_pull.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    class LogConsumerBase;

    /// Told what the log consumer learns. Attaches to the consumer in its
    /// constructor and detaches in its destructor: declaring one as a member
    /// after the consumer is the whole wiring. Both bodies are inline like
    /// every other abstract class of the components: the library is built
    /// without RTTI and an out-of-line destructor would leave the typeinfo
    /// the RTTI-enabled executables reference undefined.
    class AbsLogConsumerListener
    {
      public:
        /// @param consumer consumer to listen to; must outlive the listener
        explicit AbsLogConsumerListener(LogConsumerBase &consumer);

        virtual ~AbsLogConsumerListener();

        AbsLogConsumerListener(const AbsLogConsumerListener &) = delete;
        AbsLogConsumerListener &operator=(const AbsLogConsumerListener &) = delete;

        /// @brief One node's module table is whole, or one of its modules
        ///        moved.
        /// @param nodeId node the table belongs to
        /// @param modules the table, in table order
        virtual void onModules(std::uint32_t nodeId,
                               std::span<const mark4_LogModuleInfo> modules) = 0;

        /// @brief One line of a node the consumer follows.
        /// @param nodeId node it came from
        /// @param line the line
        virtual void onLine(std::uint32_t nodeId, const mark4_Log &line) = 0;

        /// @brief A node went down: everything the consumer held of it is
        ///        gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        LogConsumerBase &m_consumer; ///< where this listener is attached
    };

    /// Another node's log as a ground node holds it: it opens one entry per
    /// node whose identity the directory learns, subscribes to its lines and
    /// walks its module table one page at a time. The storage is the derived
    /// template's; this holds a span over it, so every composition sizes the
    /// table for itself without a second class of behaviour.
    class LogConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
    {
      public:
        /// Body tags this handler consumes: the line stream, the table
        /// pages, one module's description and the subscription the node
        /// holds. Never the request tag: the provider of the same concept
        /// claims that one, and both live on the gateway.
        static constexpr std::array<pb_size_t, 4> TAGS = {mark4_Envelope_log_tag,
                                                          mark4_Envelope_log_modules_tag,
                                                          mark4_Envelope_log_module_info_tag,
                                                          mark4_Envelope_log_subscription_tag};

        /// Node kinds that carry a LogProvider. Nothing is asked of any
        /// other kind: it would acknowledge the request and drop it.
        static constexpr std::array<mark4_NodeKind, 4> KINDS = {mark4_NodeKind_FIRMWARE,
                                                                mark4_NodeKind_DRONE_SIM,
                                                                mark4_NodeKind_RELAY,
                                                                mark4_NodeKind_GATEWAY};

        /// Modules kept per node: what the gateway's own node table carries.
        static constexpr std::size_t MAX_MODULES = 32U;

        /// Listeners one consumer may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 2U;

        /// What the consumer holds of one node.
        struct Entry
        {
            std::uint32_t id = 0U;               ///< node id
            bool subscribed = false;             ///< the node took the subscribe
            std::uint32_t subscribeRequest = 0U; ///< subscribe waiting for its answer, 0 none
            TablePull<mark4_LogModuleInfo, MAX_MODULES> modules; ///< the module table walk
        };

        /// @param messenger messenger the lines and pages come from and the
        ///        requests leave by; must outlive the consumer
        /// @param directory directory whose identities open the entries;
        ///        must outlive the consumer
        /// @param storage the entry table, owned by the derived template
        LogConsumerBase(Messenger &messenger,
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

        /// @brief Pulls one node's module table again, from the first page.
        /// @param id node to ask
        /// @return true when the page request was taken
        bool refresh(std::uint32_t id)
        {
            Entry *const entry = lookup(id);
            if (entry == nullptr)
            {
                return false;
            }
            entry->modules.reset();
            return askPage(*entry);
        }

        /// @brief Moves one module's level on one node. The answer is that
        ///        module's description, which lands in the table.
        /// @param id node to ask
        /// @param moduleId module to move
        /// @param level level to set
        /// @return true when the request was taken
        bool setLevel(std::uint32_t id, std::uint32_t moduleId, mark4_LogLevel level)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_log_set_level_tag;
            ask.body.log_set_level.module_id = moduleId;
            ask.body.log_set_level.level = level;
            ++m_requests;
            return request(id, ask) != 0U;
        }

        /// @brief A node said what it is: a node carrying a log provider is
        ///        opened, subscribed to and its table pulled. A node already
        ///        open is left alone, so an announce that only changed a
        ///        name does not restart the walk.
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

        /// @brief One line, one table page, one module description, or the
        ///        subscription the node holds.
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
            switch (envelope.which_body)
            {
                case mark4_Envelope_log_subscription_tag:
                    entry->subscribed = envelope.body.log_subscription.enabled;
                    entry->subscribeRequest = 0U;
                    return true;
                case mark4_Envelope_log_modules_tag:
                    onPage(*entry, envelope.body.log_modules);
                    return true;
                case mark4_Envelope_log_module_info_tag:
                    onModuleInfo(*entry, envelope.body.log_module_info);
                    return true;
                case mark4_Envelope_log_tag:
                    for (std::size_t index = 0U; index < m_listenerCount; ++index)
                    {
                        m_listeners[index]->onLine(src, envelope.body.log);
                    }
                    return true;
                default:
                    return false;
            }
        }

        /// @brief A node went down: its entry goes and the listeners are
        ///        told. The ids of a module table are only stable while the
        ///        node runs, so nothing of it is kept.
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

        /// @brief A request was never acknowledged: a page request abandons
        ///        the walk, a subscribe leaves the stream unheld.
        /// @param dst node it went to
        /// @param requestId id it carried
        void onRequestFailed(std::uint32_t dst, std::uint32_t requestId) override
        {
            Entry *const entry = lookup(dst);
            if (entry == nullptr)
            {
                return;
            }
            if (entry->modules.requestId() == requestId)
            {
                entry->modules.abandon();
                Module().warn("%08lx never answered for its module table",
                              static_cast<unsigned long>(dst));
                return;
            }
            if (entry->subscribeRequest == requestId)
            {
                entry->subscribeRequest = 0U;
                Module().warn("%08lx never took the log subscribe",
                              static_cast<unsigned long>(dst));
            }
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
        friend class AbsLogConsumerListener;

        /// @return the logging module of the consumer. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_LOG_CONSUMER, "log/consumer"};
            return MODULE;
        }

        /// @brief Opens one node: subscribes to its lines and starts its
        ///        table. A node already open is left alone.
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
            ask.which_body = mark4_Envelope_log_subscribe_tag;
            ask.body.log_subscribe.enabled = true;
            entry.subscribeRequest = request(id, ask);
            ++m_requests;
            static_cast<void>(askPage(entry));
        }

        /// @brief Asks one node for the page its walk waits on.
        /// @param entry node to ask
        /// @return true when the request was taken
        bool askPage(Entry &entry)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_log_modules_request_tag;
            ask.body.log_modules_request.cursor = entry.modules.cursor();
            ++m_requests;
            entry.modules.setRequestId(request(entry.id, ask));
            return entry.modules.requestId() != 0U;
        }

        /// @brief Merges one page and asks for the next, or publishes the
        ///        table when it is whole.
        /// @param entry node the page came from
        /// @param page the page
        void onPage(Entry &entry, const mark4_LogModules &page)
        {
            if (!entry.modules.applyPage(
                    page.total,
                    page.cursor,
                    std::span<const mark4_LogModuleInfo>(page.modules, page.modules_count)))
            {
                return; // a duplicate or a stale answer
            }
            if (!entry.modules.complete())
            {
                static_cast<void>(askPage(entry));
                return;
            }
            tellModules(entry);
        }

        /// @brief Replaces one module of a whole table with the description
        ///        the node just sent. A table still being walked is left
        ///        alone: the page that carries that module is still to come.
        /// @param entry node the description came from
        /// @param info the module, as it stands
        void onModuleInfo(Entry &entry, const mark4_LogModuleInfo &info)
        {
            if (!entry.modules.complete())
            {
                return;
            }
            for (mark4_LogModuleInfo &known : entry.modules.items())
            {
                if (known.id == info.id)
                {
                    known = info;
                    tellModules(entry);
                    return;
                }
            }
        }

        /// @brief Hands one node's table to every listener.
        /// @param entry the node
        void tellModules(const Entry &entry)
        {
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onModules(entry.id, entry.modules.items());
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
        void attach(AbsLogConsumerListener &listener)
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
        void detach(AbsLogConsumerListener &listener)
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

        std::span<Entry> m_entries;                                        ///< table, not owned
        std::size_t m_count = 0U;                                          ///< entries in use
        std::array<AbsLogConsumerListener *, MAX_LISTENERS> m_listeners{}; ///< attached, in order
        std::size_t m_listenerCount = 0U;                                  ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        std::uint32_t m_requests = 0U;          ///< requests started
        std::uint32_t m_dropped = 0U;           ///< nodes that found the table full
    };

    /// The log consumer with its storage: N nodes followed at once, a
    /// constant of the composition.
    ///
    /// @tparam N nodes the consumer follows at most
    template <std::size_t N> class LogConsumer final : public LogConsumerBase
    {
      public:
        /// @param messenger messenger to attach to
        /// @param directory directory to listen to
        LogConsumer(Messenger &messenger, DiscoveryDirectory &directory)
            : LogConsumerBase(messenger, directory, m_storage)
        {
        }

      private:
        std::array<Entry, N> m_storage{}; ///< the entries the base spans
    };

    inline AbsLogConsumerListener::AbsLogConsumerListener(LogConsumerBase &consumer)
        : m_consumer(consumer)
    {
        m_consumer.attach(*this);
    }

    inline AbsLogConsumerListener::~AbsLogConsumerListener()
    {
        m_consumer.detach(*this);
    }
} // namespace mark4
