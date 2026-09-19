#pragma once

/// @file
/// @brief What a ground node keeps of another node's telemetry: its measure
///        table, pulled page by page, the configuration as the node applied
///        it, and the sample stream while it is subscribed.

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
#include "telemetry/registry.hpp"

namespace mark4
{
    class TelemetryConsumerBase;

    /// Told what the telemetry consumer learns. Attaches to the consumer in
    /// its constructor and detaches in its destructor: declaring one as a
    /// member after the consumer is the whole wiring. Both bodies are inline
    /// like every other abstract class of the components: the library is
    /// built without RTTI and an out-of-line destructor would leave the
    /// typeinfo the RTTI-enabled executables reference undefined.
    class AbsTelemetryConsumerListener
    {
      public:
        /// @param consumer consumer to listen to; must outlive the listener
        explicit AbsTelemetryConsumerListener(TelemetryConsumerBase &consumer);

        virtual ~AbsTelemetryConsumerListener();

        AbsTelemetryConsumerListener(const AbsTelemetryConsumerListener &) = delete;
        AbsTelemetryConsumerListener &operator=(const AbsTelemetryConsumerListener &) = delete;

        /// @brief One node's measure table is whole.
        /// @param nodeId node the table belongs to
        /// @param descriptors the table, in table order: an index is the
        ///        wire id of the measure
        virtual void onTable(std::uint32_t nodeId,
                             std::span<const mark4_TelemetryDescriptor> descriptors) = 0;

        /// @brief One node's configuration, as the node applied it.
        /// @param nodeId the node
        /// @param config what it enabled and how often it samples
        /// @param subscribed true while this consumer holds the stream
        virtual void onConfig(std::uint32_t nodeId,
                              const mark4_TelemetryConfig &config,
                              bool subscribed) = 0;

        /// @brief One sampling instant of a node the consumer subscribed to.
        /// @param nodeId node it came from
        /// @param data the values
        virtual void onSamples(std::uint32_t nodeId, const mark4_TelemetryData &data) = 0;

        /// @brief A node went down: everything the consumer held of it is
        ///        gone.
        /// @param nodeId the node
        virtual void onForgotten(std::uint32_t nodeId) = 0;

      private:
        TelemetryConsumerBase &m_consumer; ///< where this listener is attached
    };

    /// Another node's telemetry as a ground node holds it: it opens one
    /// entry per drone whose identity the directory learns and walks its
    /// measure table one page at a time. It does not subscribe to the
    /// samples on its own: a consumer asks for the stream when it wants it.
    /// The storage is the derived template's; this holds a span over it, so
    /// every composition sizes the table for itself without a second class
    /// of behaviour.
    class TelemetryConsumerBase : public AbsMessageHandler, public AbsDirectoryListener
    {
      public:
        /// Body tags this handler consumes: the table pages, the sample
        /// stream, the configuration in effect and the subscription the node
        /// holds. Never the request tags: the provider of the same concept
        /// claims those, and both may live on one node.
        static constexpr std::array<pb_size_t, 4> TAGS = {
            mark4_Envelope_telemetry_descriptors_tag,
            mark4_Envelope_telemetry_data_tag,
            mark4_Envelope_telemetry_config_tag,
            mark4_Envelope_telemetry_subscription_tag};

        /// Node kinds that carry a TelemetryProvider. Nothing is asked of
        /// any other kind: it would acknowledge the request and drop it.
        static constexpr std::array<mark4_NodeKind, 2> KINDS = {mark4_NodeKind_FIRMWARE,
                                                                mark4_NodeKind_DRONE_SIM};

        /// Measures a configuration may name at once, from the wire bound.
        static constexpr std::size_t MAX_ENABLED =
            sizeof(mark4_TelemetryConfigure::ids) / sizeof(mark4_TelemetryConfigure::ids[0]);

        /// Listeners one consumer may hold; init() fails past that.
        static constexpr std::size_t MAX_LISTENERS = 2U;

        /// What the consumer holds of one node.
        struct Entry
        {
            std::uint32_t id = 0U; ///< node id
            /// The measure table walk: an index is the wire id of a measure.
            TablePull<mark4_TelemetryDescriptor, MAX_TELEMETRY_ENTRIES> table;
            mark4_TelemetryConfig config = ///< the last configuration heard
                mark4_TelemetryConfig_init_zero;
            bool hasConfig = false;              ///< a configuration arrived
            bool subscribed = false;             ///< the node took the subscribe
            std::uint32_t subscribeRequest = 0U; ///< subscribe waiting for its answer, 0 none
        };

        /// @param messenger messenger the pages and samples come from and
        ///        the requests leave by; must outlive the consumer
        /// @param directory directory whose identities open the entries;
        ///        must outlive the consumer
        /// @param storage the entry table, owned by the derived template
        TelemetryConsumerBase(Messenger &messenger,
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

        /// @brief Pulls one node's measure table again, from the first page.
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

        /// @brief Replaces one node's configuration: what its stream carries
        /// and how often. The node answers with what it applied.
        /// @param id node to ask
        /// @param ids measures to enable, at most MAX_ENABLED of them
        /// @param periodMs sampling period asked for [ms], 0 stops the
        ///        samples without touching the subscriptions
        /// @return true when the request was taken
        bool configure(std::uint32_t id, std::span<const std::uint32_t> ids, std::uint32_t periodMs)
        {
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_telemetry_configure_tag;
            mark4_TelemetryConfigure &config = ask.body.telemetry_configure;
            config.period_ms = periodMs;
            for (const std::uint32_t measure : ids)
            {
                if (config.ids_count >= MAX_ENABLED)
                {
                    break;
                }
                config.ids[config.ids_count] = measure;
                ++config.ids_count;
            }
            ++m_requests;
            return request(id, ask) != 0U;
        }

        /// @brief Takes one node's sample stream, or lets it go.
        /// @param id node to ask
        /// @param enabled true to hold the stream
        /// @return true when the request was taken
        bool subscribe(std::uint32_t id, bool enabled)
        {
            Entry *const entry = lookup(id);
            if (entry == nullptr)
            {
                return false;
            }
            mark4_Envelope ask = mark4_Envelope_init_zero;
            ask.which_body = mark4_Envelope_telemetry_subscribe_tag;
            ask.body.telemetry_subscribe.enabled = enabled;
            ++m_requests;
            entry->subscribeRequest = request(id, ask);
            return entry->subscribeRequest != 0U;
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

        /// @brief One table page, the configuration in effect, one batch of
        ///        samples, or the subscription the node holds.
        /// @param src node it came from
        /// @param envelope the message
        /// @param nowUs instant of the poll that delivered it [us], unused:
        ///        the samples carry their own timestamp
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
                case mark4_Envelope_telemetry_descriptors_tag:
                    onPage(*entry, envelope.body.telemetry_descriptors);
                    return true;
                case mark4_Envelope_telemetry_config_tag:
                    entry->config = envelope.body.telemetry_config;
                    entry->hasConfig = true;
                    tellConfig(*entry);
                    return true;
                case mark4_Envelope_telemetry_subscription_tag:
                    entry->subscribed = envelope.body.telemetry_subscription.enabled;
                    entry->subscribeRequest = 0U;
                    tellConfig(*entry);
                    return true;
                case mark4_Envelope_telemetry_data_tag:
                    for (std::size_t index = 0U; index < m_listenerCount; ++index)
                    {
                        m_listeners[index]->onSamples(src, envelope.body.telemetry_data);
                    }
                    return true;
                default:
                    return false;
            }
        }

        /// @brief A node went down: its entry goes and the listeners are
        ///        told. The ids of a measure table are only stable while the
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
            if (entry->table.requestId() == requestId)
            {
                entry->table.abandon();
                Module().warn("%08lx never answered for its measure table",
                              static_cast<unsigned long>(dst));
                return;
            }
            if (entry->subscribeRequest == requestId)
            {
                entry->subscribeRequest = 0U;
                Module().warn("%08lx never took the telemetry subscribe",
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
        friend class AbsTelemetryConsumerListener;

        /// @return the logging module of the consumer. A function-local
        ///         static because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TELEMETRY_CONSUMER, "telemetry/consumer"};
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
            ask.which_body = mark4_Envelope_telemetry_list_request_tag;
            ask.body.telemetry_list_request.cursor = entry.table.cursor();
            ++m_requests;
            entry.table.setRequestId(request(entry.id, ask));
            return entry.table.requestId() != 0U;
        }

        /// @brief Merges one page and asks for the next, or publishes the
        ///        table when it is whole.
        /// @param entry node the page came from
        /// @param page the page
        void onPage(Entry &entry, const mark4_TelemetryDescriptors &page)
        {
            if (!entry.table.applyPage(page.total,
                                       page.cursor,
                                       std::span<const mark4_TelemetryDescriptor>(
                                           page.descriptors, page.descriptors_count)))
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

        /// @brief Hands one node's configuration to every listener.
        /// @param entry the node
        void tellConfig(const Entry &entry)
        {
            for (std::size_t index = 0U; index < m_listenerCount; ++index)
            {
                m_listeners[index]->onConfig(entry.id, entry.config, entry.subscribed);
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
        void attach(AbsTelemetryConsumerListener &listener)
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
        void detach(AbsTelemetryConsumerListener &listener)
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
        /// Listeners attached, in order.
        std::array<AbsTelemetryConsumerListener *, MAX_LISTENERS> m_listeners{};
        std::size_t m_listenerCount = 0U;       ///< listeners attached
        std::uint32_t m_listenersOverflow = 0U; ///< live listeners that found the table full
        std::uint32_t m_requests = 0U;          ///< requests started
        std::uint32_t m_dropped = 0U;           ///< nodes that found the table full
    };

    /// The telemetry consumer with its storage: N nodes followed at once, a
    /// constant of the composition.
    ///
    /// @tparam N nodes the consumer follows at most
    template <std::size_t N> class TelemetryConsumer final : public TelemetryConsumerBase
    {
      public:
        /// @param messenger messenger to attach to
        /// @param directory directory to listen to
        TelemetryConsumer(Messenger &messenger, DiscoveryDirectory &directory)
            : TelemetryConsumerBase(messenger, directory, m_storage)
        {
        }

      private:
        std::array<Entry, N> m_storage{}; ///< the entries the base spans
    };

    inline AbsTelemetryConsumerListener::AbsTelemetryConsumerListener(
        TelemetryConsumerBase &consumer)
        : m_consumer(consumer)
    {
        m_consumer.attach(*this);
    }

    inline AbsTelemetryConsumerListener::~AbsTelemetryConsumerListener()
    {
        m_consumer.detach(*this);
    }
} // namespace mark4
