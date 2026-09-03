/// @file
/// @brief The telemetry wire adapter: what it publishes of the registry,
///        what it accepts of an enable, and how it paces and batches the
///        samples it streams to the one node that asked.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "platform_common/telemetry_service.hpp"
#include "protocol/envelope.hpp"
#include "recording_link.hpp"
#include "telemetry/registry.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    constexpr std::uint32_t NODE_SELF = 0x7E1E0000U;
    constexpr std::uint32_t NODE_GROUND = 0x67000001U;
    constexpr std::uint32_t NODE_OTHER = 0x67000002U;
    constexpr std::uint32_t MIN_PERIOD_MS = 10U;
    constexpr std::uint64_t T0_US = 1'000'000U;
    constexpr std::uint64_t US_PER_MS = 1000U;

    /// A transport over a recording link: the service answers on it and the
    /// test reads back the payloads and where they went.
    class Wire
    {
      public:
        Wire()
        {
            static_cast<void>(m_transport.addLink(m_link));
        }

        /// @return transport the service under test answers on
        mark4::Transport &transport()
        {
            return m_transport;
        }

        /// @brief Makes the transport learn one node, so a unicast to it can
        ///        actually leave: a node nobody ever heard from has no
        ///        address and every send to it is refused.
        /// @param node node to learn
        void learn(std::uint32_t node)
        {
            m_link.deliver(node, NODE_SELF, {0x00U});
            m_transport.poll(T0_US, nullptr, nullptr);
            REQUIRE(m_transport.isAlive(node));
            m_link.clear();
        }

        /// @return every frame sent so far, headers included
        [[nodiscard]] const std::vector<mark4::RecordedFrame> &frames() const
        {
            return m_link.frames();
        }

        /// @brief Forgets everything recorded so far.
        void clear()
        {
            m_link.clear();
        }

        /// @param index frame to decode
        /// @return the Envelope it carries
        [[nodiscard]] mark4_Envelope envelope(std::size_t index) const
        {
            const std::optional<mark4_Envelope> decoded = m_link.envelope(index);
            REQUIRE(decoded.has_value());
            return *decoded;
        }

      private:
        mark4::RecordingLink m_link;             ///< the medium
        mark4::Transport m_transport{NODE_SELF}; ///< what the service holds
    };

    /// A handful of measures with known names, so a test knows what the
    /// table it pulls is supposed to contain. The registry is process-wide
    /// and every other object of this binary registers into it, so the tests
    /// below locate their own measures by name instead of assuming ids.
    class Measures
    {
      public:
        /// Measures this fixture adds to the registry.
        static constexpr std::size_t COUNT = 3U;

        Measures()
        {
            m_values[0] = 1.5f;
            m_values[1] = -2.5f;
            m_values[2] = 3.5f;
        }

        /// @param index which of the three
        /// @param value new value
        void set(std::size_t index, float value)
        {
            m_values[index] = value;
        }

      private:
        std::array<float, COUNT> m_values{};
        mark4::TelemetryEntry m_first{"test/svc_a", mark4::TelemetryUnit::M, m_values[0]};
        mark4::TelemetryEntry m_second{"test/svc_b", mark4::TelemetryUnit::RAD_PER_S, m_values[1]};
        mark4::TelemetryEntry m_third{"test/svc_c", mark4::TelemetryUnit::COUNT, m_values[2]};
    };

    /// Enough measures to fill several descriptor pages and more than one
    /// sample message: the two bounds the batching is about.
    class ManyMeasures
    {
      public:
        /// Measures this fixture adds to the registry.
        static constexpr std::size_t COUNT = 40U;

        ManyMeasures()
        {
            for (std::size_t index = 0U; index < COUNT; ++index)
            {
                m_values[index] = static_cast<float>(index);
                // The name has to outlive the entry, so the fixture owns the
                // characters and the entry keeps the pointer, exactly like a
                // literal would.
                static_cast<void>(std::snprintf(
                    m_names[index].data(), m_names[index].size(), "test/many_%02zu", index));
                m_entries[index].emplace(
                    m_names[index].data(), mark4::TelemetryUnit::UNITLESS, m_values[index]);
            }
        }

        /// @param index which measure
        /// @return its name
        [[nodiscard]] const char *name(std::size_t index) const
        {
            return m_names[index].data();
        }

      private:
        std::array<float, COUNT> m_values{};
        std::array<std::array<char, 16U>, COUNT> m_names{};
        std::array<std::optional<mark4::TelemetryEntry>, COUNT> m_entries{};
    };

    /// @brief Pulls the whole table, page by page, the way the gateway does.
    /// @param service service to ask
    /// @param wire link the pages come back on
    /// @return the descriptors of the whole table, in id order
    std::vector<mark4_TelemetryDescriptor> pullTable(mark4::TelemetryService &service, Wire &wire)
    {
        std::vector<mark4_TelemetryDescriptor> all;
        std::uint32_t cursor = 0U;
        std::uint32_t total = 0U;
        do
        {
            wire.clear();
            mark4_Envelope request = mark4_Envelope_init_zero;
            request.which_body = mark4_Envelope_telemetry_list_request_tag;
            request.body.telemetry_list_request.cursor = cursor;
            REQUIRE(service.handle(request, NODE_GROUND, T0_US));
            REQUIRE(wire.frames().size() == 1U);

            const mark4_Envelope answer = wire.envelope(0U);
            REQUIRE(answer.which_body == mark4_Envelope_telemetry_descriptors_tag);
            const mark4_TelemetryDescriptors &page = answer.body.telemetry_descriptors;
            REQUIRE(page.cursor == cursor);
            total = page.total;
            for (pb_size_t index = 0U; index < page.descriptors_count; ++index)
            {
                all.push_back(page.descriptors[index]);
            }
            cursor += page.descriptors_count;
        } while (cursor < total);
        REQUIRE(all.size() == total);
        return all;
    }

    /// @param table descriptors to search
    /// @param name measure to locate
    /// @return its wire id
    std::uint32_t idOf(const std::vector<mark4_TelemetryDescriptor> &table, const char *name)
    {
        for (const mark4_TelemetryDescriptor &descriptor : table)
        {
            if (std::string(descriptor.name) == name)
            {
                return descriptor.id;
            }
        }
        FAIL("no measure named " << name);
        return 0U;
    }

    /// @param ids measures to enable
    /// @param periodMs period asked for
    /// @return one TelemetryEnable
    mark4_Envelope makeEnable(const std::vector<std::uint32_t> &ids, std::uint32_t periodMs)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_telemetry_enable_tag;
        mark4_TelemetryEnable &enable = envelope.body.telemetry_enable;
        enable.period_ms = periodMs;
        for (const std::uint32_t id : ids)
        {
            enable.ids[enable.ids_count] = id;
            ++enable.ids_count;
        }
        return envelope;
    }
} // namespace

TEST_CASE("the table is published page by page, the last page closing it")
{
    Measures measures;
    ManyMeasures many;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());

    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    REQUIRE(table.size() == service.entryCount());
    REQUIRE(table.size() >= Measures::COUNT + ManyMeasures::COUNT);
    // The id of a measure is its index in the frozen table, so the ids the
    // pages carry are exactly 0..total-1, in order.
    for (std::size_t index = 0U; index < table.size(); ++index)
    {
        REQUIRE(table[index].id == index);
    }
    // The names and units come back as the registry has them, and in
    // construction order.
    const std::uint32_t idB = idOf(table, "test/svc_b");
    REQUIRE(table[idB].unit == mark4_TelemetryUnit_TELEMETRY_UNIT_RAD_PER_S);
    REQUIRE(idOf(table, "test/svc_c") == idB + 1U);
    REQUIRE(idOf(table, many.name(1U)) == idOf(table, many.name(0U)) + 1U);

    // Several pages were needed, and only the last one was short: paging
    // must not cost a round trip per measure.
    REQUIRE(table.size() > mark4::TelemetryService::DESCRIPTORS_PER_PAGE);
}

TEST_CASE("a page asked past the end comes back empty, carrying the total")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());

    mark4_Envelope request = mark4_Envelope_init_zero;
    request.which_body = mark4_Envelope_telemetry_list_request_tag;
    request.body.telemetry_list_request.cursor = 100000U;
    REQUIRE(service.handle(request, NODE_GROUND, T0_US));

    const mark4_TelemetryDescriptors &page = wire.envelope(0U).body.telemetry_descriptors;
    REQUIRE(page.descriptors_count == 0U);
    REQUIRE(page.total == service.entryCount());
    // Unicast to whoever asked: discovery is a conversation, not a broadcast.
    REQUIRE(!wire.frames()[0].broadcast);
    REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);
}

TEST_CASE("an enable is acknowledged with the period and the count in effect")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");

    SECTION("a period under the floor is clamped up to it")
    {
        wire.clear();
        REQUIRE(service.handle(makeEnable({idA}, 1U), NODE_GROUND, T0_US));
        const mark4_TelemetryAck &ack = wire.envelope(0U).body.telemetry_ack;
        REQUIRE(ack.period_ms == MIN_PERIOD_MS);
        REQUIRE(ack.enabled == 1U);
        REQUIRE(service.periodMs() == MIN_PERIOD_MS);
        REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);
    }
    SECTION("a period over the ceiling is clamped down to it")
    {
        wire.clear();
        REQUIRE(service.handle(
            makeEnable({idA}, 10U * mark4::TelemetryService::MAX_PERIOD_MS), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_ack.period_ms ==
                mark4::TelemetryService::MAX_PERIOD_MS);
    }
    SECTION("unknown ids are dropped and the rest is kept")
    {
        wire.clear();
        REQUIRE(service.handle(makeEnable({idA, 100000U, 100001U}, 50U), NODE_GROUND, T0_US));
        const mark4_TelemetryAck &ack = wire.envelope(0U).body.telemetry_ack;
        REQUIRE(ack.enabled == 1U);
        REQUIRE(ack.period_ms == 50U);
        REQUIRE(service.streaming());
    }
    SECTION("an enable with no known id at all stops the stream")
    {
        REQUIRE(service.handle(makeEnable({idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(service.streaming());
        wire.clear();
        REQUIRE(service.handle(makeEnable({100000U}, 50U), NODE_GROUND, T0_US));
        const mark4_TelemetryAck &ack = wire.envelope(0U).body.telemetry_ack;
        REQUIRE(ack.period_ms == 0U);
        REQUIRE(ack.enabled == 0U);
        REQUIRE(!service.streaming());
    }
    SECTION("period 0 stops the stream")
    {
        REQUIRE(service.handle(makeEnable({idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(service.streaming());
        wire.clear();
        REQUIRE(service.handle(makeEnable({idA}, 0U), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_ack.period_ms == 0U);
        REQUIRE(!service.streaming());
        // And nothing goes out afterwards, whatever the frames say.
        wire.clear();
        service.sample(T0_US + 1'000'000U);
        REQUIRE(wire.frames().empty());
    }
    SECTION("the same measure listed twice is one measure")
    {
        wire.clear();
        REQUIRE(service.handle(makeEnable({idA, idA, idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_ack.enabled == 1U);
    }
}

TEST_CASE("the samples follow the period, unicast to the subscriber")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    const std::uint32_t idC = idOf(table, "test/svc_c");

    REQUIRE(service.handle(makeEnable({idC, idA}, 50U), NODE_GROUND, T0_US));
    wire.clear();

    // The first sample goes out on the very next frame: a subscriber that
    // asked for a slow period must not wait a whole one to see anything.
    service.sample(T0_US + 2000U);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(!wire.frames()[0].broadcast);
    REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);

    const mark4_TelemetryData &data = wire.envelope(0U).body.telemetry_data;
    REQUIRE(data.timestamp_us == T0_US + 2000U);
    REQUIRE(data.values_count == 2U);
    // Ascending by id whatever order the request listed them in.
    REQUIRE(data.values[0].id == idA);
    REQUIRE(data.values[0].value == 1.5f);
    REQUIRE(data.values[1].id == idC);
    REQUIRE(data.values[1].value == 3.5f);

    // Nothing until the period elapsed, then exactly one message.
    wire.clear();
    for (std::uint64_t at = T0_US + 4000U; at < T0_US + 52000U; at += 2000U)
    {
        service.sample(at);
    }
    REQUIRE(wire.frames().empty());
    measures.set(0U, 9.25f);
    service.sample(T0_US + 52000U);
    REQUIRE(wire.frames().size() == 1U);
    // Read at the sampling instant, not copied at the enable.
    REQUIRE(wire.envelope(0U).body.telemetry_data.values[0].value == 9.25f);
}

TEST_CASE("a sampling instant wider than one message is split, timestamp kept")
{
    Measures measures;
    ManyMeasures many;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::size_t perMessage = mark4::TelemetryService::VALUES_PER_MESSAGE;
    REQUIRE(service.entryCount() > perMessage);

    // One more measure than a message holds.
    std::vector<std::uint32_t> ids;
    for (std::size_t index = 0U; index <= perMessage; ++index)
    {
        ids.push_back(static_cast<std::uint32_t>(index));
    }
    REQUIRE(service.handle(makeEnable(ids, 50U), NODE_GROUND, T0_US));
    wire.clear();

    service.sample(T0_US + 2000U);
    REQUIRE(wire.frames().size() == 2U);
    const mark4_Envelope firstEnvelope = wire.envelope(0U);
    const mark4_Envelope secondEnvelope = wire.envelope(1U);
    const mark4_TelemetryData &first = firstEnvelope.body.telemetry_data;
    const mark4_TelemetryData &second = secondEnvelope.body.telemetry_data;
    REQUIRE(first.values_count == perMessage);
    REQUIRE(second.values_count == 1U);
    // Both halves describe the same instant: a consumer joins them on it.
    REQUIRE(first.timestamp_us == T0_US + 2000U);
    REQUIRE(second.timestamp_us == T0_US + 2000U);
    REQUIRE(second.values[0].id == perMessage);
}

TEST_CASE("the stream stops when the subscriber stops repeating its enable")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");

    REQUIRE(service.handle(makeEnable({idA}, MIN_PERIOD_MS), NODE_GROUND, T0_US));
    const std::uint64_t timeout = mark4::TelemetryService::SUBSCRIBER_TIMEOUT_US;

    // Just inside the window the stream still runs.
    wire.clear();
    service.sample(T0_US + timeout);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(service.streaming());

    // One microsecond past it the stream stops, and nothing more goes out.
    wire.clear();
    service.sample(T0_US + timeout + 1U);
    REQUIRE(wire.frames().empty());
    REQUIRE(!service.streaming());
    REQUIRE(service.periodMs() == 0U);
    service.sample(T0_US + 2U * timeout);
    REQUIRE(wire.frames().empty());

    // A fresh enable arms it again.
    REQUIRE(service.handle(makeEnable({idA}, MIN_PERIOD_MS), NODE_GROUND, T0_US + 2U * timeout));
    REQUIRE(service.streaming());
}

TEST_CASE("a keepalive keeps the stream alive without restarting the pacing")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    const std::uint32_t periodMs = 50U;

    REQUIRE(service.handle(makeEnable({idA}, periodMs), NODE_GROUND, T0_US));
    service.sample(T0_US);
    wire.clear();

    // The subscriber repeats its enable once per second, well inside the
    // period: that must not make the samples come faster.
    for (std::uint64_t at = T0_US + 2000U; at <= T0_US + 1'000'000U; at += 2000U)
    {
        if (at % 1'000'000U == 0U)
        {
            REQUIRE(service.handle(makeEnable({idA}, periodMs), NODE_GROUND, at));
        }
        service.sample(at);
    }
    const std::size_t acks = 1U;
    const std::size_t expected = 1'000'000U / (periodMs * US_PER_MS);
    // One ack for the keepalive, and one sample per period, no more.
    REQUIRE(wire.frames().size() == expected + acks);
}

TEST_CASE("an enable from another node takes the stream over")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(service, wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    const std::uint32_t idC = idOf(table, "test/svc_c");

    REQUIRE(service.handle(makeEnable({idA}, MIN_PERIOD_MS), NODE_GROUND, T0_US));
    REQUIRE(service.subscriber() == NODE_GROUND);

    // Last writer wins: one active stream per drone, and it is the new
    // node's set and period that apply.
    wire.clear();
    REQUIRE(service.handle(makeEnable({idA, idC}, 100U), NODE_OTHER, T0_US + 1000U));
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
    REQUIRE(service.subscriber() == NODE_OTHER);
    REQUIRE(service.enabledCount() == 2U);
    REQUIRE(service.periodMs() == 100U);

    wire.clear();
    service.sample(T0_US + 2000U);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
    REQUIRE(wire.envelope(0U).body.telemetry_data.values_count == 2U);
}

TEST_CASE("a message that is not a telemetry request is left to its owner")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryService service(wire.transport(), MIN_PERIOD_MS);
    REQUIRE(service.init());

    mark4_Envelope rc = mark4_Envelope_init_zero;
    rc.which_body = mark4_Envelope_rc_tag;
    REQUIRE(!service.handle(rc, NODE_GROUND, T0_US));
    REQUIRE(wire.frames().empty());
}
