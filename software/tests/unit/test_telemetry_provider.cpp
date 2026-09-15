/// @file
/// @brief The telemetry provider, driven through the wire: what it publishes
///        of the registry, what it accepts of a configuration, and how it
///        paces and batches the samples it streams to the nodes that
///        subscribed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "recording_link.hpp"
#include "telemetry/provider.hpp"
#include "telemetry/registry.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D00U;
    /// Requests these benches keep at once. The gateway's size, because
    /// nothing here acknowledges what the provider answers and every
    /// unanswered request holds an entry.
    constexpr std::size_t PENDING_REQUESTS = mark4::Messenger::HUB_PENDING_REQUESTS;
    constexpr std::uint32_t NODE_SELF = 0x7E1E0000U;
    constexpr std::uint32_t NODE_GROUND = 0x67000001U;
    constexpr std::uint32_t NODE_OTHER = 0x67000002U;
    constexpr std::uint32_t NODE_THIRD = 0x67000003U;
    constexpr std::uint32_t MIN_PERIOD_MS = 10U;
    constexpr std::uint64_t T0_US = 1'000'000U;
    constexpr std::uint64_t US_PER_MS = 1000U;

    /// A messenger over a transport over a recording link: the requests
    /// come in through the link, the service answers on it, and the test
    /// reads back the payloads and where they went.
    class Wire
    {
      public:
        Wire()
        {
            static_cast<void>(m_transport.addLink(m_link));
        }

        /// @return messenger the service under test attaches to
        mark4::Messenger &messenger()
        {
            return m_messenger;
        }

        /// @brief Makes the transport learn one node, so a unicast to it can
        ///        actually leave: a node nobody ever heard from has no
        ///        address and every send to it is refused.
        /// @param node node to learn
        void learn(std::uint32_t node)
        {
            m_link.deliver(node, NODE_SELF, {0x00U});
            m_messenger.poll(T0_US);
            REQUIRE(m_transport.isAlive(node));
            m_link.clear();
        }

        /// @brief Delivers one message to this node and polls once, the way
        ///        a flight loop does.
        /// @param envelope message to deliver
        /// @param src node it comes from
        /// @param nowUs instant of the poll [us]
        /// @return true when a handler acted on it
        bool request(const mark4_Envelope &envelope, std::uint32_t src, std::uint64_t nowUs)
        {
            std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE, 0U);
            std::size_t size = 0U;
            REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
            bytes.resize(size);
            m_link.deliver(src, NODE_SELF, bytes);
            const std::uint32_t handled = m_messenger.handled();
            m_messenger.poll(nowUs);
            return m_messenger.handled() == handled + 1U;
        }

        /// @return messages the messenger found no handler for
        [[nodiscard]] std::uint32_t unhandled() const
        {
            return m_messenger.unhandled();
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
        mark4::RecordingLink m_link;                                     ///< the medium
        mark4::Transport m_transport{NODE_SELF, BOOT_ID};                ///< this node
        std::array<mark4::PendingRequest, PENDING_REQUESTS> m_pending{}; ///< requests kept
        mark4::Messenger m_messenger{m_transport, m_pending}; ///< what the service attaches to
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
    /// @param wire the wire the requests go in and the pages come back on
    /// @return the descriptors of the whole table, in id order
    std::vector<mark4_TelemetryDescriptor> pullTable(Wire &wire)
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
            REQUIRE(wire.request(request, NODE_GROUND, T0_US));
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
    /// @return one TelemetryConfig
    mark4_Envelope makeConfig(const std::vector<std::uint32_t> &ids, std::uint32_t periodMs)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_telemetry_config_tag;
        mark4_TelemetryConfig &config = envelope.body.telemetry_config;
        config.period_ms = periodMs;
        for (const std::uint32_t id : ids)
        {
            config.ids[config.ids_count] = id;
            ++config.ids_count;
        }
        return envelope;
    }

    /// @param enabled what to ask for
    /// @return one TelemetrySubscribe
    mark4_Envelope makeSubscribe(bool enabled)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_telemetry_subscribe_tag;
        envelope.body.telemetry_subscribe.enabled = enabled;
        return envelope;
    }

    /// @brief Takes the sample stream of the provider under test, checking
    ///        the answer says so.
    /// @param wire the wire the request goes in and the answer comes back on
    /// @param node node taking the stream
    void subscribe(Wire &wire, std::uint32_t node)
    {
        wire.clear();
        REQUIRE(wire.request(makeSubscribe(true), node, T0_US));
        REQUIRE(wire.frames().size() == 1U);
        const mark4_Envelope answer = wire.envelope(0U);
        REQUIRE(answer.which_body == mark4_Envelope_telemetry_subscribe_tag);
        REQUIRE(answer.body.telemetry_subscribe.enabled);
        wire.clear();
    }
} // namespace

TEST_CASE("the table is published page by page, the last page closing it")
{
    Measures measures;
    ManyMeasures many;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());

    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    REQUIRE(table.size() == provider.entryCount());
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
    REQUIRE(table.size() > mark4::TelemetryProvider::DESCRIPTORS_PER_PAGE);
}

TEST_CASE("a page asked past the end comes back empty, carrying the total")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());

    mark4_Envelope request = mark4_Envelope_init_zero;
    request.which_body = mark4_Envelope_telemetry_list_request_tag;
    request.body.telemetry_list_request.cursor = 100000U;
    REQUIRE(wire.request(request, NODE_GROUND, T0_US));

    const mark4_TelemetryDescriptors &page = wire.envelope(0U).body.telemetry_descriptors;
    REQUIRE(page.descriptors_count == 0U);
    REQUIRE(page.total == provider.entryCount());
    // Unicast to whoever asked: discovery is a conversation, not a broadcast.
    REQUIRE(!wire.frames()[0].broadcast);
    REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);
}

TEST_CASE("a configuration is answered with the period and the ids in effect")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");

    SECTION("a period under the floor is clamped up to it")
    {
        wire.clear();
        REQUIRE(wire.request(makeConfig({idA}, 1U), NODE_GROUND, T0_US));
        const mark4_TelemetryConfig &applied = wire.envelope(0U).body.telemetry_config;
        REQUIRE(applied.period_ms == MIN_PERIOD_MS);
        REQUIRE(applied.ids_count == 1U);
        REQUIRE(applied.ids[0] == idA);
        REQUIRE(provider.periodMs() == MIN_PERIOD_MS);
        REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);
    }
    SECTION("a period over the ceiling is clamped down to it")
    {
        wire.clear();
        REQUIRE(wire.request(
            makeConfig({idA}, 10U * mark4::TelemetryProvider::MAX_PERIOD_MS), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_config.period_ms ==
                mark4::TelemetryProvider::MAX_PERIOD_MS);
    }
    SECTION("unknown ids are dropped and the rest is kept")
    {
        wire.clear();
        REQUIRE(wire.request(makeConfig({idA, 100000U, 100001U}, 50U), NODE_GROUND, T0_US));
        const mark4_TelemetryConfig &applied = wire.envelope(0U).body.telemetry_config;
        REQUIRE(applied.ids_count == 1U);
        REQUIRE(applied.period_ms == 50U);
        REQUIRE(provider.streaming());
    }
    SECTION("a configuration with no known id at all stops the samples")
    {
        REQUIRE(wire.request(makeConfig({idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(provider.streaming());
        wire.clear();
        REQUIRE(wire.request(makeConfig({100000U}, 50U), NODE_GROUND, T0_US));
        const mark4_TelemetryConfig &applied = wire.envelope(0U).body.telemetry_config;
        REQUIRE(applied.period_ms == 0U);
        REQUIRE(applied.ids_count == 0U);
        REQUIRE(!provider.streaming());
    }
    SECTION("period 0 stops the samples")
    {
        REQUIRE(wire.request(makeConfig({idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(provider.streaming());
        wire.clear();
        REQUIRE(wire.request(makeConfig({idA}, 0U), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_config.period_ms == 0U);
        REQUIRE(!provider.streaming());
        // And nothing goes out afterwards, whatever the frames say.
        wire.clear();
        provider.sample(T0_US + 1'000'000U);
        REQUIRE(wire.frames().empty());
    }
    SECTION("the same measure listed twice is one measure")
    {
        wire.clear();
        REQUIRE(wire.request(makeConfig({idA, idA, idA}, 50U), NODE_GROUND, T0_US));
        REQUIRE(wire.envelope(0U).body.telemetry_config.ids_count == 1U);
    }
}

TEST_CASE("the samples follow the period, unicast to each subscriber")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    const std::uint32_t idC = idOf(table, "test/svc_c");

    subscribe(wire, NODE_GROUND);
    REQUIRE(wire.request(makeConfig({idC, idA}, 50U), NODE_GROUND, T0_US));
    wire.clear();

    // The first sample goes out on the very next frame: a subscriber that
    // asked for a slow period must not wait a whole one to see anything.
    provider.sample(T0_US + 2000U);
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
        provider.sample(at);
    }
    REQUIRE(wire.frames().empty());
    measures.set(0U, 9.25f);
    provider.sample(T0_US + 52000U);
    REQUIRE(wire.frames().size() == 1U);
    // Read at the sampling instant, not copied at the configuration.
    REQUIRE(wire.envelope(0U).body.telemetry_data.values[0].value == 9.25f);
}

TEST_CASE("a sampling instant wider than one message is split, timestamp kept")
{
    Measures measures;
    ManyMeasures many;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::size_t perMessage = mark4::TelemetryProvider::VALUES_PER_MESSAGE;
    REQUIRE(provider.entryCount() > perMessage);

    // One more measure than a message holds.
    std::vector<std::uint32_t> ids;
    for (std::size_t index = 0U; index <= perMessage; ++index)
    {
        ids.push_back(static_cast<std::uint32_t>(index));
    }
    subscribe(wire, NODE_GROUND);
    REQUIRE(wire.request(makeConfig(ids, 50U), NODE_GROUND, T0_US));
    wire.clear();

    provider.sample(T0_US + 2000U);
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

TEST_CASE("the stream stops when the subscriber goes down")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");

    subscribe(wire, NODE_GROUND);
    provider.sample(T0_US);
    REQUIRE(wire.request(makeConfig({idA}, MIN_PERIOD_MS), NODE_GROUND, T0_US));

    // While the node is there the samples go to it.
    wire.clear();
    provider.sample(T0_US + MIN_PERIOD_MS * US_PER_MS);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(provider.subscribers() == 1U);

    // A subscriber that vanished stops the stream: presence says who is
    // still there, and nothing is repeated to say so.
    provider.onNodeDown(NODE_GROUND);
    REQUIRE(provider.subscribers() == 0U);
    wire.clear();
    provider.sample(T0_US + 2U * MIN_PERIOD_MS * US_PER_MS);
    REQUIRE(wire.frames().empty());
    // The configuration is the node's own and outlives its subscribers.
    REQUIRE(provider.streaming());

    // A node that subscribes again gets the stream back.
    subscribe(wire, NODE_OTHER);
    provider.sample(T0_US + 3U * MIN_PERIOD_MS * US_PER_MS);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
}

TEST_CASE("the configuration instant is the last frame sampled, not the poll's clock")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    /// A poll far in the future of the frames: the two clocks of the sim.
    const std::uint64_t poll = T0_US + 10'000'000U;

    // The frames are on one time base, the poll on another: a configuration
    // delivered by a poll far in the future of the frames must not be timed
    // against that clock, or every frame would look like the first one
    // under it and the period would never hold.
    subscribe(wire, NODE_GROUND);
    provider.sample(T0_US);
    REQUIRE(wire.request(makeConfig({idA}, 50U), NODE_GROUND, poll));
    wire.clear();

    // The first sample goes out at once, the next one only after the period.
    provider.sample(T0_US + 2000U);
    REQUIRE(wire.frames().size() == 1U);
    wire.clear();
    provider.sample(T0_US + 4000U);
    REQUIRE(wire.frames().empty());
    provider.sample(T0_US + 52000U);
    REQUIRE(wire.frames().size() == 1U);
}

TEST_CASE("every subscriber gets the stream and the configuration as applied")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    wire.learn(NODE_THIRD);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());
    const std::vector<mark4_TelemetryDescriptor> table = pullTable(wire);
    const std::uint32_t idA = idOf(table, "test/svc_a");
    const std::uint32_t idC = idOf(table, "test/svc_c");

    subscribe(wire, NODE_GROUND);
    subscribe(wire, NODE_OTHER);
    REQUIRE(provider.subscribers() == 2U);

    // One configuration per node, last writer wins; the node that asked is
    // answered and the other subscriber is told the same thing.
    REQUIRE(wire.request(makeConfig({idA}, MIN_PERIOD_MS), NODE_GROUND, T0_US));
    wire.clear();
    REQUIRE(wire.request(makeConfig({idA, idC}, 100U), NODE_OTHER, T0_US + 1000U));
    REQUIRE(wire.frames().size() == 2U);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
    REQUIRE(wire.frames()[1].header.dst == NODE_GROUND);
    for (std::size_t index = 0U; index < 2U; ++index)
    {
        const mark4_TelemetryConfig &applied = wire.envelope(index).body.telemetry_config;
        REQUIRE(applied.period_ms == 100U);
        REQUIRE(applied.ids_count == 2U);
    }
    REQUIRE(provider.enabledCount() == 2U);
    REQUIRE(provider.periodMs() == 100U);

    // And the samples go to both of them, once each.
    wire.clear();
    provider.sample(T0_US + 2000U);
    REQUIRE(wire.frames().size() == 2U);
    REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);
    REQUIRE(wire.frames()[1].header.dst == NODE_OTHER);
    REQUIRE(wire.envelope(0U).body.telemetry_data.values_count == 2U);

    // A full table refuses one more, and says so.
    wire.clear();
    REQUIRE(wire.request(makeSubscribe(true), NODE_THIRD, T0_US + 2000U));
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(!wire.envelope(0U).body.telemetry_subscribe.enabled);
    REQUIRE(provider.subscribers() == mark4::TelemetryProvider::MAX_SUBSCRIBERS);

    // Giving the stream back is answered with false and stops the samples.
    wire.clear();
    REQUIRE(wire.request(makeSubscribe(false), NODE_GROUND, T0_US + 3000U));
    REQUIRE(!wire.envelope(0U).body.telemetry_subscribe.enabled);
    REQUIRE(provider.subscribers() == 1U);
    wire.clear();
    provider.sample(T0_US + 102000U);
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
}

TEST_CASE("a message that is not a telemetry request is left to its owner")
{
    Measures measures;
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::TelemetryProvider provider(wire.messenger(), MIN_PERIOD_MS);
    REQUIRE(provider.init());

    // Nothing claimed the tag: the messenger counts it, the provider never
    // sees it and answers nothing.
    mark4_Envelope rc = mark4_Envelope_init_zero;
    rc.which_body = mark4_Envelope_rc_tag;
    REQUIRE(!wire.request(rc, NODE_GROUND, T0_US));
    REQUIRE(wire.unhandled() == 1U);
    REQUIRE(wire.frames().empty());
}
