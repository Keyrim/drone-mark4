/// @file
/// @brief The log library: modules register themselves, a level gates the
///        formatting, a prefix moves a whole area, the text truncates, and
///        the provider rate limits the line stream, pages the module table
///        and moves one level from the wire.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "log/provider.hpp"
#include "log/wire.hpp"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "recording_link.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    constexpr std::uint16_t TEST_BASE = 0x7000U;

    mark4::LogModule IMU{TEST_BASE + 0U, "test/platform/imu"};
    mark4::LogModule BARO{TEST_BASE + 1U, "test/platform/baro"};
    mark4::LogModule FLIGHT{TEST_BASE + 2U, "test/flight/core"};

    /// Remembers every record, as strings: the record's pointers die with
    /// the write.
    class RecordingSink final : public mark4::AbsLogSink
    {
      public:
        struct Line
        {
            std::uint16_t moduleId;
            std::string moduleName;
            mark4::LogLevel level;
            std::uint64_t timestampUs;
            std::string text;
        };

        void write(const mark4::LogRecord &record) override
        {
            lines.push_back({record.moduleId,
                             record.moduleName,
                             record.level,
                             record.timestampUs,
                             record.text});
        }

        std::vector<Line> lines;
    };

    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D01U;
    constexpr std::uint32_t NODE_SELF = 0x109E0000U;
    constexpr std::uint32_t NODE_GROUND = 0x67000001U;
    constexpr std::uint32_t NODE_OTHER = 0x67000002U;
    constexpr std::uint64_t T0_US = 1'000'000U;

    /// A messenger over a transport over a recording link: the requests come
    /// in through the link, the provider answers on it, and the test reads
    /// back the payloads and where they went. The pending table is the
    /// gateway's size because nothing here acknowledges what the provider
    /// sends, and every unanswered request holds an entry.
    class Wire
    {
      public:
        Wire()
        {
            static_cast<void>(m_transport.addLink(m_link));
        }

        /// @return messenger the provider under test attaches to
        mark4::Messenger &messenger()
        {
            return m_messenger;
        }

        /// @brief Makes the transport learn one node, so a unicast to it can
        ///        actually leave.
        /// @param node node to learn
        void learn(std::uint32_t node)
        {
            m_link.deliver(node, NODE_SELF, {0x00U});
            m_messenger.poll(T0_US);
            REQUIRE(m_transport.isAlive(node));
            m_link.clear();
        }

        /// @brief Delivers one message to this node and polls once.
        /// @param envelope message to deliver
        /// @param src node it comes from
        /// @return true when a handler acted on it
        bool request(const mark4_Envelope &envelope, std::uint32_t src)
        {
            std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE, 0U);
            std::size_t size = 0U;
            REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
            bytes.resize(size);
            m_link.deliver(src, NODE_SELF, bytes);
            const std::uint32_t handled = m_messenger.handled();
            m_messenger.poll(T0_US);
            return m_messenger.handled() == handled + 1U;
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
        mark4::RecordingLink m_link;                      ///< the medium
        mark4::Transport m_transport{NODE_SELF, BOOT_ID}; ///< this node
        std::array<mark4::PendingRequest, mark4::Messenger::HUB_PENDING_REQUESTS>
            m_pending{};                                      ///< requests kept
        mark4::Messenger m_messenger{m_transport, m_pending}; ///< what the provider attaches to
    };

    /// @param enabled what to ask for
    /// @return one LogSubscribe
    mark4_Envelope makeSubscribe(bool enabled)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_log_subscribe_tag;
        envelope.body.log_subscribe.enabled = enabled;
        return envelope;
    }

    std::uint64_t g_nowUs = 0U;

    std::uint64_t testClock(void *context)
    {
        static_cast<void>(context);
        return g_nowUs;
    }

    /// Registers a sink and the test clock for one test, undoes both.
    class Session
    {
      public:
        explicit Session(mark4::AbsLogSink &sink)
            : m_sink(sink)
        {
            mark4::logSetClock(&testClock, nullptr);
            REQUIRE(mark4::logAddSink(sink));
            mark4::logSetLevelByPrefix("test/", mark4::LogModule::DEFAULT_LEVEL);
        }

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;
        Session(Session &&) = delete;
        Session &operator=(Session &&) = delete;

        ~Session()
        {
            mark4::logRemoveSink(m_sink);
            mark4::logSetClock(nullptr, nullptr);
            mark4::logSetLevelByPrefix("test/", mark4::LogModule::DEFAULT_LEVEL);
        }

      private:
        mark4::AbsLogSink &m_sink;
    };

    mark4::LogRecord recordOf(std::uint64_t timestampUs, const char *text)
    {
        mark4::LogRecord record;
        record.moduleId = IMU.id();
        record.moduleName = IMU.name();
        record.level = mark4::LogLevel::INFO;
        record.timestampUs = timestampUs;
        record.text = text;
        return record;
    }
} // namespace

TEST_CASE("every module declared is in the registry, once, with its id and name")
{
    std::size_t seen = 0U;
    for (const mark4::LogModule *module = mark4::logModules(); module != nullptr;
         module = module->next())
    {
        if (module->id() >= TEST_BASE && module->id() < TEST_BASE + 3U)
        {
            ++seen;
        }
    }
    CHECK(seen == 3U);
    CHECK(mark4::logModuleCount() >= 4U); // the three above plus log/core
    CHECK(mark4::logFindModule(TEST_BASE + 1U) == &BARO);
    CHECK(std::string(mark4::logFindModule(mark4::LOG_MODULE_CORE)->name()) == "log/core");
    CHECK(mark4::logFindModule(0xFFFFU) == nullptr);
    CHECK(!mark4::logSetLevel(0xFFFFU, mark4::LogLevel::TRACE));
}

TEST_CASE("a line below the module level is dropped before it is formatted")
{
    RecordingSink sink;
    const Session session(sink);
    g_nowUs = 42U;

    IMU.debug("%s", "hidden");
    IMU.trace("%d", 1);
    CHECK(sink.lines.empty());

    IMU.info("found at 0x%02X", 0x68U);
    IMU.warn("%s", "slow");
    REQUIRE(sink.lines.size() == 2U);
    CHECK(sink.lines[0].moduleId == IMU.id());
    CHECK(sink.lines[0].moduleName == "test/platform/imu");
    CHECK(sink.lines[0].level == mark4::LogLevel::INFO);
    CHECK(sink.lines[0].timestampUs == 42U);
    CHECK(sink.lines[0].text == "found at 0x68");
    CHECK(sink.lines[1].level == mark4::LogLevel::WARN);

    REQUIRE(mark4::logSetLevel(IMU.id(), mark4::LogLevel::TRACE));
    IMU.trace("t");
    CHECK(sink.lines.size() == 3U);
    IMU.setLevel(mark4::LogLevel::ERROR);
    IMU.warn("w");
    IMU.error("e");
    REQUIRE(sink.lines.size() == 4U);
    CHECK(sink.lines[3].text == "e");
}

TEST_CASE("a prefix moves every module under it and nothing else")
{
    RecordingSink sink;
    const Session session(sink);

    CHECK(mark4::logSetLevelByPrefix("test/platform", mark4::LogLevel::DEBUG) == 2U);
    CHECK(IMU.level() == mark4::LogLevel::DEBUG);
    CHECK(BARO.level() == mark4::LogLevel::DEBUG);
    CHECK(FLIGHT.level() == mark4::LogLevel::INFO);
    CHECK(mark4::logSetLevelByPrefix("test/nothing", mark4::LogLevel::TRACE) == 0U);
    CHECK(mark4::logSetLevelByPrefix("test/", mark4::LogLevel::WARN) == 3U);
    FLIGHT.info("hidden");
    FLIGHT.warn("shown");
    REQUIRE(sink.lines.size() == 1U);
    CHECK(sink.lines[0].text == "shown");
}

TEST_CASE("the text is cut at MAX_TEXT and never overflows")
{
    RecordingSink sink;
    const Session session(sink);

    const std::string longText(3U * mark4::LogModule::MAX_TEXT, 'x');
    IMU.info("%s", longText.c_str());
    REQUIRE(sink.lines.size() == 1U);
    CHECK(sink.lines[0].text.size() == mark4::LogModule::MAX_TEXT);
    CHECK(sink.lines[0].text == longText.substr(0U, mark4::LogModule::MAX_TEXT));
}

TEST_CASE("two sinks at most, each removed on request")
{
    RecordingSink first;
    RecordingSink second;
    RecordingSink third;
    const Session session(first);
    REQUIRE(mark4::logAddSink(second));
    CHECK(!mark4::logAddSink(third));
    IMU.info("both");
    CHECK(first.lines.size() == 1U);
    CHECK(second.lines.size() == 1U);
    mark4::logRemoveSink(second);
    IMU.info("one");
    CHECK(first.lines.size() == 2U);
    CHECK(second.lines.size() == 1U);
}

TEST_CASE("the provider sends a Log envelope to its subscribers and rate limits it")
{
    Wire wire;
    wire.learn(NODE_GROUND);
    mark4::LogProvider provider(wire.messenger());

    // The subscribe is answered with the subscription as it stands; nothing
    // of the stream leaves before it (the end of this case checks the other way
    // round, when the subscriber goes away).
    REQUIRE(wire.request(makeSubscribe(true), NODE_GROUND));
    REQUIRE(wire.frames().size() == 1U);
    CHECK(wire.envelope(0U).which_body == mark4_Envelope_log_subscription_tag);
    CHECK(wire.envelope(0U).body.log_subscription.enabled);
    CHECK(provider.subscribers() == 1U);
    wire.clear();

    provider.write(recordOf(1'000U, "first"));
    REQUIRE(wire.frames().size() == 1U);
    CHECK(wire.frames()[0].header.dst == NODE_GROUND);
    CHECK(wire.envelope(0U).which_body == mark4_Envelope_log_tag);
    CHECK(wire.envelope(0U).body.log.module_id == IMU.id());
    CHECK(wire.envelope(0U).body.log.level == mark4_LogLevel_INFO);
    CHECK(wire.envelope(0U).body.log.timestamp_us == 1'000U);
    CHECK(std::string(wire.envelope(0U).body.log.text) == "first");

    // Fill the second: everything past the limit is counted, not sent.
    for (std::uint32_t i = 0U; i < 2U * mark4::LogProvider::MAX_LINES_PER_SECOND; ++i)
    {
        provider.write(recordOf(2'000U, "burst"));
    }
    CHECK(wire.frames().size() == mark4::LogProvider::MAX_LINES_PER_SECOND);
    CHECK(provider.dropped() == mark4::LogProvider::MAX_LINES_PER_SECOND + 1U);

    // The next second opens with the count, as a WARN of log/core, then the
    // line itself.
    provider.write(recordOf(1'000U + mark4::LogProvider::WINDOW_US, "later"));
    REQUIRE(wire.frames().size() == mark4::LogProvider::MAX_LINES_PER_SECOND + 2U);
    const std::size_t noticeAt = wire.frames().size() - 2U;
    const mark4_Log &notice = wire.envelope(noticeAt).body.log;
    CHECK(notice.module_id == mark4::LOG_MODULE_CORE);
    CHECK(notice.level == mark4_LogLevel_WARN);
    CHECK(std::string(notice.text) == "51 lines dropped by the rate limit");
    CHECK(std::string(wire.envelope(wire.frames().size() - 1U).body.log.text) == "later");

    // A node that goes down stops holding the stream.
    wire.clear();
    provider.onNodeDown(NODE_GROUND);
    CHECK(provider.subscribers() == 0U);
    provider.write(recordOf(2U * mark4::LogProvider::WINDOW_US, "unheard again"));
    CHECK(wire.frames().empty());
}

TEST_CASE("the module table goes out one page per request and a level moves from the wire")
{
    RecordingSink sink;
    const Session session(sink);
    Wire wire;
    wire.learn(NODE_GROUND);
    wire.learn(NODE_OTHER);
    mark4::LogProvider provider(wire.messenger());

    // The table is walked one page per request: the requester paces it, and
    // the last page is the one where cursor + modules == total.
    const std::size_t total = mark4::logModuleCount();
    std::size_t listed = 0U;
    std::size_t pages = 0U;
    bool sawBaro = false;
    do
    {
        wire.clear();
        mark4_Envelope ask = mark4_Envelope_init_zero;
        ask.which_body = mark4_Envelope_log_modules_request_tag;
        ask.body.log_modules_request.cursor = static_cast<std::uint32_t>(listed);
        REQUIRE(wire.request(ask, NODE_GROUND));
        REQUIRE(wire.frames().size() == 1U);
        CHECK(wire.frames()[0].header.dst == NODE_GROUND);
        const mark4_Envelope answer = wire.envelope(0U);
        REQUIRE(answer.which_body == mark4_Envelope_log_modules_tag);
        const mark4_LogModules &page = answer.body.log_modules;
        CHECK(page.total == total);
        CHECK(page.cursor == listed);
        for (pb_size_t i = 0U; i < page.modules_count; ++i)
        {
            if (page.modules[i].id == BARO.id())
            {
                sawBaro = true;
                CHECK(std::string(page.modules[i].name) == "test/platform/baro");
                CHECK(page.modules[i].level == mark4_LogLevel_INFO);
            }
        }
        listed += page.modules_count;
        ++pages;
    } while (listed < total);
    CHECK(listed == total);
    CHECK(sawBaro);
    CHECK(pages == (total + 7U) / 8U);

    // A page asked past the end comes back empty, carrying the total.
    wire.clear();
    mark4_Envelope far = mark4_Envelope_init_zero;
    far.which_body = mark4_Envelope_log_modules_request_tag;
    far.body.log_modules_request.cursor = 100000U;
    REQUIRE(wire.request(far, NODE_GROUND));
    CHECK(wire.envelope(0U).body.log_modules.modules_count == 0U);
    CHECK(wire.envelope(0U).body.log_modules.total == total);

    // Two nodes hold the line stream, so the one that moves a level gets the
    // module as it stands and the other is told the same thing.
    REQUIRE(wire.request(makeSubscribe(true), NODE_GROUND));
    REQUIRE(wire.request(makeSubscribe(true), NODE_OTHER));
    REQUIRE(provider.subscribers() == 2U);

    mark4_Envelope set = mark4_Envelope_init_zero;
    set.which_body = mark4_Envelope_log_set_level_tag;
    set.body.log_set_level.module_id = BARO.id();
    set.body.log_set_level.level = mark4_LogLevel_TRACE;
    wire.clear();
    REQUIRE(wire.request(set, NODE_GROUND));
    CHECK(BARO.level() == mark4::LogLevel::TRACE);
    REQUIRE(wire.frames().size() == 2U);
    CHECK(wire.frames()[0].header.dst == NODE_GROUND);
    CHECK(wire.frames()[1].header.dst == NODE_OTHER);
    for (std::size_t index = 0U; index < 2U; ++index)
    {
        const mark4_Envelope info = wire.envelope(index);
        REQUIRE(info.which_body == mark4_Envelope_log_module_info_tag);
        CHECK(info.body.log_module_info.id == BARO.id());
        CHECK(std::string(info.body.log_module_info.name) == "test/platform/baro");
        CHECK(info.body.log_module_info.level == mark4_LogLevel_TRACE);
    }

    // The same level again changes nothing, so only the requester is
    // answered: nothing moved for the others to hear.
    wire.clear();
    REQUIRE(wire.request(set, NODE_GROUND));
    REQUIRE(wire.frames().size() == 1U);
    CHECK(wire.frames()[0].header.dst == NODE_GROUND);

    // A module this node does not have: nothing to move and nothing to
    // describe.
    wire.clear();
    set.body.log_set_level.module_id = 0xFFFFU;
    REQUIRE(wire.request(set, NODE_GROUND));
    CHECK(wire.frames().empty());

    // A level a newer peer knows and this build does not: a plain integer on
    // the wire (enum_intsize IS_32), refused rather than loaded, and the
    // answer says what the module still is.
    wire.clear();
    set.body.log_set_level.module_id = BARO.id();
    const int unknownLevel = 99;
    static_assert(sizeof(set.body.log_set_level.level) == sizeof(unknownLevel));
    std::memcpy(&set.body.log_set_level.level, &unknownLevel, sizeof(unknownLevel));
    REQUIRE(wire.request(set, NODE_GROUND));
    CHECK(BARO.level() == mark4::LogLevel::TRACE);
    REQUIRE(wire.frames().size() == 1U);
    CHECK(wire.envelope(0U).body.log_module_info.level == mark4_LogLevel_TRACE);

    // Giving the stream back is answered with false and stops the lines.
    wire.clear();
    REQUIRE(wire.request(makeSubscribe(false), NODE_OTHER));
    CHECK(!wire.envelope(0U).body.log_subscription.enabled);
    CHECK(provider.subscribers() == 1U);
}
