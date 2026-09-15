#pragma once

/// @file
/// @brief The wire adapter of the telemetry registry: it freezes the leaf
///        library's list into a table, answers the discovery and
///        configuration messages of `protocol/mark4.proto`, and streams
///        batched samples to the nodes that subscribed.
///
/// Timing contract. onMessage() runs inside the messenger poll, before
/// step(); sample() is called once per flight frame, right after step() and
/// the motor push, with the frame's own timestamp. The provider never reads
/// a clock: every instant comes from the caller, exactly like the transport.
///
/// One configuration per node, never one per subscriber: what the stream
/// carries and how often is a state of the node, last writer wins, and
/// every subscriber is told when it moves.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "messaging/messenger.hpp"
#include "messaging/subscriber_table.hpp"
#include "protocol/envelope.hpp"
#include "telemetry/registry.hpp"

namespace mark4
{
    // The registry is a leaf and cannot include a generated protobuf header,
    // so its unit enum and the wire's are two independent definitions. This
    // is where they are pinned to each other, value by value.
    static_assert(static_cast<int>(TelemetryUnit::UNITLESS) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_UNITLESS);
    static_assert(static_cast<int>(TelemetryUnit::M) == mark4_TelemetryUnit_TELEMETRY_UNIT_M);
    static_assert(static_cast<int>(TelemetryUnit::M_PER_S) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_M_PER_S);
    static_assert(static_cast<int>(TelemetryUnit::M_PER_S2) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_M_PER_S2);
    static_assert(static_cast<int>(TelemetryUnit::RAD) == mark4_TelemetryUnit_TELEMETRY_UNIT_RAD);
    static_assert(static_cast<int>(TelemetryUnit::RAD_PER_S) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_RAD_PER_S);
    static_assert(static_cast<int>(TelemetryUnit::PA) == mark4_TelemetryUnit_TELEMETRY_UNIT_PA);
    static_assert(static_cast<int>(TelemetryUnit::CELSIUS) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_CELSIUS);
    static_assert(static_cast<int>(TelemetryUnit::V) == mark4_TelemetryUnit_TELEMETRY_UNIT_V);
    static_assert(static_cast<int>(TelemetryUnit::A) == mark4_TelemetryUnit_TELEMETRY_UNIT_A);
    static_assert(static_cast<int>(TelemetryUnit::US) == mark4_TelemetryUnit_TELEMETRY_UNIT_US);
    static_assert(static_cast<int>(TelemetryUnit::COUNT) ==
                  mark4_TelemetryUnit_TELEMETRY_UNIT_COUNT);
    static_assert(MAX_TELEMETRY_NAME + 1U == sizeof(mark4_TelemetryDescriptor::name),
                  "the measure name width must match the wire");

    /// Answers the telemetry requests the messenger hands it, to the node
    /// that asked, and streams what the configuration names to the nodes
    /// that subscribed.
    ///
    /// Two time bases meet here. The messenger is polled on the clock of the
    /// process (the sim polls it from the sensor wait), sample() runs on the
    /// timestamp of the flight frame, and the two differ; the stream is
    /// timed against the frames, so a configuration is stamped with the
    /// instant of the last sample(), never with the instant of the poll that
    /// delivered it.
    class TelemetryProvider final : public AbsMessageHandler
    {
      public:
        /// Body tags this handler consumes.
        static constexpr std::array<pb_size_t, 3> TAGS = {mark4_Envelope_telemetry_list_request_tag,
                                                          mark4_Envelope_telemetry_config_tag,
                                                          mark4_Envelope_telemetry_subscribe_tag};

        /// Nodes that may hold the stream at once. Two: a sampling instant
        /// is one emission per entry, on a link that also carries the flight
        /// traffic.
        static constexpr std::size_t MAX_SUBSCRIBERS = 2U;

        /// Descriptors per TelemetryDescriptors page, from the wire bound.
        static constexpr std::size_t DESCRIPTORS_PER_PAGE =
            sizeof(mark4_TelemetryDescriptors::descriptors) /
            sizeof(mark4_TelemetryDescriptors::descriptors[0]);

        /// Values per TelemetryData message, from the wire bound. A sampling
        /// instant with more enabled measures than this goes out as several
        /// messages carrying the same timestamp.
        static constexpr std::size_t VALUES_PER_MESSAGE =
            sizeof(mark4_TelemetryData::values) / sizeof(mark4_TelemetryData::values[0]);

        /// Measures the configuration may name at once, from the wire bound.
        static constexpr std::size_t MAX_ENABLED =
            sizeof(mark4_TelemetryConfig::ids) / sizeof(mark4_TelemetryConfig::ids[0]);

        /// Slowest period a consumer may ask for [ms]. Past a minute
        /// nothing is left of the stream and the request is a mistake.
        static constexpr std::uint32_t MAX_PERIOD_MS = 60000U;

        /// Microseconds in a millisecond, for the period arithmetic.
        static constexpr std::uint64_t US_PER_MS = 1000U;

        /// @param messenger messenger the requests come from and the answers
        ///        and the samples leave by; must outlive the provider
        /// @param minPeriodMs fastest period this composition accepts [ms];
        ///        what the link can carry, not what the loop can produce
        TelemetryProvider(Messenger &messenger, std::uint32_t minPeriodMs)
            : AbsMessageHandler(messenger, TAGS),
              m_messenger(messenger),
              m_minPeriodMs(minPeriodMs == 0U ? 1U : minPeriodMs)
        {
        }

        /// @brief Freezes the registry into the table this node publishes:
        ///        the id of a measure is its index in it, for the life of the
        ///        process. Call it last in App::init(), once every service
        ///        exists: entries constructed afterwards are invisible.
        /// @return false when the registry is empty, which is a composition
        ///         that has not built its services yet
        bool init()
        {
            m_count = 0U;
            std::size_t skippedNames = 0U;
            std::size_t skippedOverflow = 0U;
            for (const TelemetryEntry *entry = telemetryEntries(); entry != nullptr;
                 entry = entry->next())
            {
                if (std::strlen(entry->name()) > MAX_TELEMETRY_NAME)
                {
                    ++skippedNames;
                    continue;
                }
                if (m_count == m_entries.size())
                {
                    ++skippedOverflow;
                    continue;
                }
                m_entries[m_count] = entry;
                ++m_count;
            }
            if (skippedNames > 0U)
            {
                Module().warn("%zu measure(s) ignored: name over %zu characters",
                              skippedNames,
                              MAX_TELEMETRY_NAME);
            }
            if (skippedOverflow > 0U)
            {
                Module().warn("%zu measure(s) ignored: the table holds %zu",
                              skippedOverflow,
                              m_entries.size());
            }
            Module().info("%zu measures, %u ms floor, %zu per message",
                          m_count,
                          static_cast<unsigned>(m_minPeriodMs),
                          VALUES_PER_MESSAGE);
            return m_count > 0U;
        }

        /// @brief Answers one telemetry request, to the node that sent it.
        /// @param src node it came from: where the answer goes
        /// @param envelope decoded message
        /// @param nowUs instant of the poll that delivered it [us]
        /// @return true when the message was answered
        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            // The poll's instant is on the process clock while the stream is
            // timed on the frames: a configuration is stamped with the last
            // sample() instead (see the class comment).
            static_cast<void>(nowUs);
            switch (envelope.which_body)
            {
                case mark4_Envelope_telemetry_list_request_tag:
                    sendPage(envelope.body.telemetry_list_request.cursor, src);
                    return true;
                case mark4_Envelope_telemetry_config_tag:
                    applyConfig(envelope.body.telemetry_config, src, m_frameUs);
                    return true;
                case mark4_Envelope_telemetry_subscribe_tag:
                    applySubscribe(src, envelope.body.telemetry_subscribe.enabled);
                    return true;
                default:
                    return false;
            }
        }

        /// @brief Emits one sampling instant when the period elapsed. Call
        ///        once per flight frame with the frame's own timestamp;
        ///        nothing goes out without a configuration or without a
        ///        subscriber.
        /// @param nowUs timestamp of the frame just stepped [us]
        void sample(std::uint64_t nowUs)
        {
            m_frameUs = nowUs;
            if (!m_streaming || m_subscribers.empty())
            {
                return;
            }
            const std::uint64_t periodUs = static_cast<std::uint64_t>(m_periodMs) * US_PER_MS;
            // A batch that predates the configuration means none has gone
            // out under it, and the first one is not made to wait a whole
            // period. Both instants are on the frames' time base, which is
            // why a configuration is stamped with the last sample() and
            // never with the poll that delivered it.
            const bool sampledUnderConfig = m_lastSampleUs >= m_lastConfigUs;
            if (sampledUnderConfig && (nowUs < m_lastSampleUs || nowUs - m_lastSampleUs < periodUs))
            {
                return;
            }
            m_lastSampleUs = nowUs;
            emit(nowUs);
        }

        /// @brief A node went down: it is not listening any more. The
        ///        configuration stays: it is the node's, not its
        ///        subscribers'.
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

        /// @return measures in the frozen table
        [[nodiscard]] std::size_t entryCount() const
        {
            return m_count;
        }

        /// @return true while a configuration produces samples: measures
        ///         enabled and a period to emit them at
        [[nodiscard]] bool streaming() const
        {
            return m_streaming;
        }

        /// @return period in effect [ms], 0 when no stream is armed
        [[nodiscard]] std::uint32_t periodMs() const
        {
            return m_streaming ? m_periodMs : 0U;
        }

        /// @return measures the configuration enabled
        [[nodiscard]] std::size_t enabledCount() const
        {
            return m_enabledCount;
        }

        /// @return nodes holding the stream
        [[nodiscard]] std::size_t subscribers() const
        {
            return m_subscribers.size();
        }

        /// @return TelemetryData messages sent since construction
        [[nodiscard]] std::uint32_t messageCount() const
        {
            return m_messageCount;
        }

      private:
        /// @return the logging module of the provider. A function-local static
        ///         because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_TELEMETRY_PROVIDER, "telemetry/provider"};
            return MODULE;
        }

        /// @brief Sends one page of the table.
        /// @param cursor first id to describe; past the end sends an empty
        ///        page carrying the total, which is how a requester learns it
        ///        asked too far
        /// @param dst node the page goes to
        void sendPage(std::uint32_t cursor, std::uint32_t dst)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_telemetry_descriptors_tag;
            mark4_TelemetryDescriptors &page = envelope.body.telemetry_descriptors;
            page.total = static_cast<std::uint32_t>(m_count);
            page.cursor = cursor;
            // Clamped first, so the walk below is provably inside the
            // table whatever cursor the requester sent.
            const std::size_t first = cursor > m_count ? m_count : static_cast<std::size_t>(cursor);
            for (std::size_t index = first;
                 index < m_count && page.descriptors_count < DESCRIPTORS_PER_PAGE;
                 ++index)
            {
                mark4_TelemetryDescriptor &descriptor = page.descriptors[page.descriptors_count];
                descriptor.id = static_cast<std::uint32_t>(index);
                std::strncpy(
                    descriptor.name, m_entries[index]->name(), sizeof(descriptor.name) - 1U);
                descriptor.unit = static_cast<mark4_TelemetryUnit>(m_entries[index]->unit());
                ++page.descriptors_count;
            }
            static_cast<void>(request(dst, envelope));
        }

        /// @brief Replaces the enabled set and the period.
        /// @param config the request
        /// @param src node that sent it: where the answer goes
        /// @param nowUs instant the request is stamped with [us], on the
        ///        time base of the frames
        void applyConfig(const mark4_TelemetryConfig &config,
                         std::uint32_t src,
                         std::uint64_t nowUs)
        {
            const bool wasStreaming = m_streaming;
            std::size_t unknown = 0U;
            m_enabledCount = 0U;
            for (pb_size_t index = 0U; index < config.ids_count; ++index)
            {
                const std::uint32_t id = config.ids[index];
                if (id >= m_count || m_enabledCount == m_enabled.size())
                {
                    ++unknown;
                    continue;
                }
                insertSorted(id);
            }
            if (unknown > 0U)
            {
                Module().warn("%zu id(s) of the configuration dropped: unknown or past %zu",
                              unknown,
                              m_enabled.size());
            }

            if (config.period_ms == 0U || m_enabledCount == 0U)
            {
                // The subscriptions stand: the node has nothing to sample,
                // not nobody to sample for.
                if (wasStreaming)
                {
                    Module().info("samples stopped by %08lx", static_cast<unsigned long>(src));
                }
                stop();
                tellConfig(src);
                return;
            }

            m_periodMs = clampPeriod(config.period_ms);
            // Stamped on the frames' time base, so the first sample goes out
            // on the very next frame: a consumer that asked for a slow
            // period must not wait a whole one before seeing anything.
            m_lastConfigUs = nowUs;
            m_streaming = true;
            Module().info("%zu measures every %u ms, asked by %08lx",
                          m_enabledCount,
                          static_cast<unsigned>(m_periodMs),
                          static_cast<unsigned long>(src));
            tellConfig(src);
        }

        /// @brief Takes one subscribe request and answers it with what was
        ///        applied.
        /// @param src node that asked
        /// @param enabled what it asked for
        void applySubscribe(std::uint32_t src, bool enabled)
        {
            bool applied = false;
            if (enabled)
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
            answer.which_body = mark4_Envelope_telemetry_subscribe_tag;
            answer.body.telemetry_subscribe.enabled = applied;
            static_cast<void>(request(src, answer));
        }

        /// @brief Inserts one id into the enabled set, kept ascending: the
        ///        batches then carry the ids in a stable order whatever order
        ///        the request listed them in.
        /// @param id measure id, already known to be in the table
        void insertSorted(std::uint32_t id)
        {
            std::size_t at = 0U;
            while (at < m_enabledCount && m_enabled[at] < id)
            {
                ++at;
            }
            if (at < m_enabledCount && m_enabled[at] == id)
            {
                return; // the same measure twice is one measure
            }
            for (std::size_t index = m_enabledCount; index > at; --index)
            {
                m_enabled[index] = m_enabled[index - 1U];
            }
            m_enabled[at] = id;
            ++m_enabledCount;
        }

        /// @param requested period the consumer asked for [ms]
        /// @return the period in effect [ms]
        [[nodiscard]] std::uint32_t clampPeriod(std::uint32_t requested) const
        {
            if (requested < m_minPeriodMs)
            {
                return m_minPeriodMs;
            }
            return requested > MAX_PERIOD_MS ? MAX_PERIOD_MS : requested;
        }

        /// @brief Sends the configuration as applied to the node that asked
        ///        for it and to every other subscriber: holding the stream
        ///        means hearing what changes it.
        /// @param requester node that sent the configuration
        void tellConfig(std::uint32_t requester)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_telemetry_config_tag;
            mark4_TelemetryConfig &config = envelope.body.telemetry_config;
            config.period_ms = m_streaming ? m_periodMs : 0U;
            for (std::size_t index = 0U; index < m_enabledCount; ++index)
            {
                config.ids[config.ids_count] = m_enabled[index];
                ++config.ids_count;
            }
            static_cast<void>(request(requester, envelope));
            for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
            {
                if (m_subscribers.id(index) != requester)
                {
                    mark4_Envelope copy = envelope;
                    static_cast<void>(request(m_subscribers.id(index), copy));
                }
            }
        }

        /// @brief Reads every enabled measure and sends it to every
        ///        subscriber as one or more TelemetryData messages, all
        ///        carrying the same timestamp.
        /// @param nowUs timestamp of the frame being reported [us]
        void emit(std::uint64_t nowUs)
        {
            std::size_t at = 0U;
            while (at < m_enabledCount)
            {
                mark4_Envelope envelope = mark4_Envelope_init_zero;
                envelope.which_body = mark4_Envelope_telemetry_data_tag;
                mark4_TelemetryData &data = envelope.body.telemetry_data;
                data.timestamp_us = nowUs;
                while (at < m_enabledCount && data.values_count < VALUES_PER_MESSAGE)
                {
                    const std::uint32_t id = m_enabled[at];
                    data.values[data.values_count].id = id;
                    data.values[data.values_count].value = m_entries[id]->read();
                    ++data.values_count;
                    ++at;
                }
                // A refused send is dropped and counted by the messenger,
                // never retried: a sample is only worth its own instant.
                for (std::size_t index = 0U; index < m_subscribers.size(); ++index)
                {
                    if (m_messenger.send(m_subscribers.id(index), envelope))
                    {
                        ++m_messageCount;
                    }
                }
            }
        }

        /// @brief Stops the samples, keeping the frozen table and the
        ///        subscriptions.
        void stop()
        {
            m_streaming = false;
            m_enabledCount = 0U;
            m_periodMs = 0U;
        }

        Messenger &m_messenger;      ///< answers and samples leave by it, not owned
        std::uint32_t m_minPeriodMs; ///< fastest period this composition accepts [ms]
        /// Timestamp of the last sample(), the time base a configuration is
        /// stamped with.
        std::uint64_t m_frameUs = 0U;

        /// The frozen table: the index of an entry IS its wire id.
        std::array<const TelemetryEntry *, MAX_TELEMETRY_ENTRIES> m_entries{};
        std::size_t m_count = 0U; ///< measures in m_entries

        SubscriberTable<MAX_SUBSCRIBERS> m_subscribers;     ///< nodes holding the stream
        std::array<std::uint32_t, MAX_ENABLED> m_enabled{}; ///< enabled ids, ascending
        std::size_t m_enabledCount = 0U;                    ///< ids in m_enabled
        std::uint32_t m_periodMs = 0U;                      ///< period in effect [ms]
        /// Instant of the last configuration [us], on the frames' time base.
        std::uint64_t m_lastConfigUs = 0U;
        std::uint64_t m_lastSampleUs = 0U; ///< instant of the last batch [us]
        bool m_streaming = false;          ///< the configuration produces samples
        std::uint32_t m_messageCount = 0U; ///< TelemetryData messages sent
    };
} // namespace mark4
