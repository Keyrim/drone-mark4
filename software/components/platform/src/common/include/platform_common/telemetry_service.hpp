#pragma once

/// @file
/// @brief The wire adapter of the telemetry registry: it freezes the leaf
///        library's list into a table, answers the discovery and enable
///        messages of `protocol/mark4.proto`, and streams batched samples to
///        the one node that asked for them.
///
/// Timing contract. handle() is called from the command drain loop, before
/// step(); sample() is called once per flight frame, right after step() and
/// the motor push, with the frame's own timestamp. The service never reads a
/// clock: every instant comes from the caller, exactly like the transport.
///
/// One active stream per drone. An enable from another node takes it over
/// (last writer wins): a bench where two tools fight over the stream is a
/// bench problem, and answering both would double the traffic on a UART
/// that has none to spare.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "platform_common/envelope_io.hpp"
#include "protocol/envelope.hpp"
#include "telemetry/registry.hpp"
#include "transport/transport.hpp"

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

    /// Answers the telemetry messages a composition hands it, and streams
    /// what its subscriber asked for.
    class TelemetryService
    {
      public:
        /// Descriptors per TelemetryDescriptors page, from the wire bound.
        static constexpr std::size_t DESCRIPTORS_PER_PAGE =
            sizeof(mark4_TelemetryDescriptors::descriptors) /
            sizeof(mark4_TelemetryDescriptors::descriptors[0]);

        /// Values per TelemetryData message, from the wire bound. A sampling
        /// instant with more enabled measures than this goes out as several
        /// messages carrying the same timestamp.
        static constexpr std::size_t VALUES_PER_MESSAGE =
            sizeof(mark4_TelemetryData::values) / sizeof(mark4_TelemetryData::values[0]);

        /// Measures one subscriber may enable at once, from the wire bound.
        static constexpr std::size_t MAX_ENABLED =
            sizeof(mark4_TelemetryEnable::ids) / sizeof(mark4_TelemetryEnable::ids[0]);

        /// Slowest period a subscriber may ask for [ms]. Past a minute the
        /// keepalive is the only traffic left and the request is a mistake.
        static constexpr std::uint32_t MAX_PERIOD_MS = 60000U;

        /// Silence from the subscriber after which the stream stops [us].
        /// The subscriber repeats its enable once per second, so this is
        /// three missed keepalives, like the transport's node expiry: a
        /// ground tool that crashed must not leave a board streaming into
        /// the void for the rest of the flight.
        static constexpr std::uint64_t SUBSCRIBER_TIMEOUT_US = 3'000'000U;

        /// Microseconds in a millisecond, for the period arithmetic.
        static constexpr std::uint64_t US_PER_MS = 1000U;

        /// @param transport transport the answers and the samples leave by
        /// @param minPeriodMs fastest period this composition accepts [ms];
        ///        what the link can carry, not what the loop can produce
        TelemetryService(Transport &transport, std::uint32_t minPeriodMs)
            : m_transport(transport),
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

        /// @brief Answers one message, when it is a telemetry request.
        /// @param envelope decoded message
        /// @param src node it came from: where the answer goes
        /// @param nowUs instant of the frame this drain belongs to [us]
        /// @return true when the message was a telemetry request and was
        ///         answered, false when it belongs to someone else
        bool handle(const mark4_Envelope &envelope, std::uint32_t src, std::uint64_t nowUs)
        {
            switch (envelope.which_body)
            {
                case mark4_Envelope_telemetry_list_request_tag:
                    sendPage(envelope.body.telemetry_list_request.cursor, src);
                    return true;
                case mark4_Envelope_telemetry_enable_tag:
                    applyEnable(envelope.body.telemetry_enable, src, nowUs);
                    return true;
                default:
                    return false;
            }
        }

        /// @brief Emits one sampling instant when the period elapsed, and
        ///        stops the stream when the subscriber went silent. Call once
        ///        per flight frame with the frame's own timestamp.
        /// @param nowUs timestamp of the frame just stepped [us]
        void sample(std::uint64_t nowUs)
        {
            if (!m_streaming)
            {
                return;
            }
            if (nowUs > m_lastEnableUs && nowUs - m_lastEnableUs > SUBSCRIBER_TIMEOUT_US)
            {
                Module().info("subscriber %08lx silent, stream stopped",
                              static_cast<unsigned long>(m_subscriber));
                stop();
                return;
            }
            const std::uint64_t periodUs = static_cast<std::uint64_t>(m_periodMs) * US_PER_MS;
            if (m_sampled && (nowUs < m_lastSampleUs || nowUs - m_lastSampleUs < periodUs))
            {
                return;
            }
            m_lastSampleUs = nowUs;
            m_sampled = true;
            emit(nowUs);
        }

        /// @return measures in the frozen table
        [[nodiscard]] std::size_t entryCount() const
        {
            return m_count;
        }

        /// @return true while a subscriber is being served
        [[nodiscard]] bool streaming() const
        {
            return m_streaming;
        }

        /// @return period in effect [ms], 0 when no stream is armed
        [[nodiscard]] std::uint32_t periodMs() const
        {
            return m_streaming ? m_periodMs : 0U;
        }

        /// @return measures enabled by the current subscriber
        [[nodiscard]] std::size_t enabledCount() const
        {
            return m_enabledCount;
        }

        /// @return node the samples go to, BROADCAST_NODE when none
        [[nodiscard]] std::uint32_t subscriber() const
        {
            return m_streaming ? m_subscriber : BROADCAST_NODE;
        }

        /// @return TelemetryData messages sent since construction
        [[nodiscard]] std::uint32_t messageCount() const
        {
            return m_messageCount;
        }

      private:
        /// @return the logging module of the service. A function-local static
        ///         because the class is header-only: one instance per
        ///         process, however many compositions include it.
        static LogModule &Module()
        {
            static LogModule MODULE{LOG_MODULE_PLATFORM_TELEMETRY, "platform/telemetry"};
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
            static_cast<void>(sendEnvelope(m_transport, dst, envelope));
        }

        /// @brief Replaces the enabled set and (re)arms the stream.
        /// @param enable the request
        /// @param src node that sent it: the new subscriber
        /// @param nowUs instant the request was drained at [us]
        void applyEnable(const mark4_TelemetryEnable &enable,
                         std::uint32_t src,
                         std::uint64_t nowUs)
        {
            const bool wasStreaming = m_streaming;
            const std::uint32_t previous = m_subscriber;
            std::size_t unknown = 0U;
            m_enabledCount = 0U;
            for (pb_size_t index = 0U; index < enable.ids_count; ++index)
            {
                const std::uint32_t id = enable.ids[index];
                if (id >= m_count || m_enabledCount == m_enabled.size())
                {
                    ++unknown;
                    continue;
                }
                insertSorted(id);
            }
            if (unknown > 0U)
            {
                Module().warn("%zu id(s) of the enable dropped: unknown or past %zu",
                              unknown,
                              m_enabled.size());
            }

            if (enable.period_ms == 0U || m_enabledCount == 0U)
            {
                if (wasStreaming)
                {
                    Module().info("stream stopped by %08lx", static_cast<unsigned long>(src));
                }
                stop();
                sendAck(src, 0U, 0U);
                return;
            }

            m_periodMs = clampPeriod(enable.period_ms);
            m_subscriber = src;
            m_lastEnableUs = nowUs;
            m_streaming = true;
            // The first sample goes out on the very next frame: a subscriber
            // that asked for a slow period must not wait a whole one before
            // seeing anything.
            m_sampled = false;
            if (!wasStreaming)
            {
                Module().info("stream to %08lx: %zu measures every %u ms",
                              static_cast<unsigned long>(src),
                              m_enabledCount,
                              static_cast<unsigned>(m_periodMs));
            }
            else if (previous != src)
            {
                Module().info("stream taken over by %08lx (was %08lx)",
                              static_cast<unsigned long>(src),
                              static_cast<unsigned long>(previous));
            }
            sendAck(src, m_periodMs, static_cast<std::uint32_t>(m_enabledCount));
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

        /// @param requested period the subscriber asked for [ms]
        /// @return the period in effect [ms]
        [[nodiscard]] std::uint32_t clampPeriod(std::uint32_t requested) const
        {
            if (requested < m_minPeriodMs)
            {
                return m_minPeriodMs;
            }
            return requested > MAX_PERIOD_MS ? MAX_PERIOD_MS : requested;
        }

        /// @brief Answers one enable with what was applied.
        /// @param dst node that sent the enable
        /// @param periodMs period in effect, 0 when the stream stopped
        /// @param enabled measures kept
        void sendAck(std::uint32_t dst, std::uint32_t periodMs, std::uint32_t enabled)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_telemetry_ack_tag;
            envelope.body.telemetry_ack.period_ms = periodMs;
            envelope.body.telemetry_ack.enabled = enabled;
            static_cast<void>(sendEnvelope(m_transport, dst, envelope));
        }

        /// @brief Reads every enabled measure and sends it as one or more
        ///        TelemetryData messages, all carrying the same timestamp.
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
                // A refused send is dropped and counted by the transport,
                // never retried: a sample is only worth its own instant.
                if (sendEnvelope(m_transport, m_subscriber, envelope))
                {
                    ++m_messageCount;
                }
            }
        }

        /// @brief Disarms the stream, keeping the frozen table.
        void stop()
        {
            m_streaming = false;
            m_enabledCount = 0U;
            m_periodMs = 0U;
            m_sampled = false;
        }

        Transport &m_transport;      ///< answers and samples leave by it, not owned
        std::uint32_t m_minPeriodMs; ///< fastest period this composition accepts [ms]

        /// The frozen table: the index of an entry IS its wire id.
        std::array<const TelemetryEntry *, MAX_TELEMETRY_ENTRIES> m_entries{};
        std::size_t m_count = 0U; ///< measures in m_entries

        std::array<std::uint32_t, MAX_ENABLED> m_enabled{}; ///< enabled ids, ascending
        std::size_t m_enabledCount = 0U;                    ///< ids in m_enabled
        std::uint32_t m_subscriber = BROADCAST_NODE;        ///< node the samples go to
        std::uint32_t m_periodMs = 0U;                      ///< period in effect [ms]
        std::uint64_t m_lastEnableUs = 0U;                  ///< instant of the last enable [us]
        std::uint64_t m_lastSampleUs = 0U;                  ///< instant of the last batch [us]
        bool m_streaming = false;                           ///< a subscriber is being served
        bool m_sampled = false;                             ///< a batch went out since the enable
        std::uint32_t m_messageCount = 0U;                  ///< TelemetryData messages sent
    };
} // namespace mark4
