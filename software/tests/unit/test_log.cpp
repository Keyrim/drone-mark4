/// @file
/// @brief The log library: modules register themselves, a level gates the
///        formatting, a prefix moves a whole area, the text truncates, the
///        transport sink rate limits and reports it, the wire helpers page
///        the table and carry out a LogControl.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "log/wire.hpp"

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

    /// Decodes everything a TransportSink or logPublishModules() sends.
    struct Captured
    {
        std::vector<mark4_Envelope> envelopes;
        bool accept = true;

        static bool Send(void *context, const std::uint8_t *data, std::size_t size)
        {
            auto *self = static_cast<Captured *>(context);
            mark4_Envelope envelope;
            REQUIRE(mark4::decodeEnvelope(data, size, envelope));
            self->envelopes.push_back(envelope);
            return self->accept;
        }
    };

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

TEST_CASE("the transport sink encodes a Log envelope and rate limits it")
{
    Captured captured;
    mark4::TransportSink sink(&Captured::Send, &captured);

    sink.write(recordOf(1'000U, "first"));
    REQUIRE(captured.envelopes.size() == 1U);
    CHECK(captured.envelopes[0].which_body == mark4_Envelope_log_tag);
    CHECK(captured.envelopes[0].body.log.module_id == IMU.id());
    CHECK(captured.envelopes[0].body.log.level == mark4_LogLevel_INFO);
    CHECK(captured.envelopes[0].body.log.timestamp_us == 1'000U);
    CHECK(std::string(captured.envelopes[0].body.log.text) == "first");

    // Fill the second: everything past the limit is counted, not sent.
    for (std::uint32_t i = 0U; i < 2U * mark4::TransportSink::MAX_LINES_PER_SECOND; ++i)
    {
        sink.write(recordOf(2'000U, "burst"));
    }
    CHECK(captured.envelopes.size() == mark4::TransportSink::MAX_LINES_PER_SECOND);
    CHECK(sink.dropped() == mark4::TransportSink::MAX_LINES_PER_SECOND + 1U);

    // The next second opens with the count, as a WARN of log/core, then the
    // line itself.
    sink.write(recordOf(1'000U + mark4::TransportSink::WINDOW_US, "later"));
    REQUIRE(captured.envelopes.size() == mark4::TransportSink::MAX_LINES_PER_SECOND + 2U);
    const mark4_Log &notice = captured.envelopes[captured.envelopes.size() - 2U].body.log;
    CHECK(notice.module_id == mark4::LOG_MODULE_CORE);
    CHECK(notice.level == mark4_LogLevel_WARN);
    CHECK(std::string(notice.text) == "51 lines dropped by the rate limit");
    CHECK(std::string(captured.envelopes.back().body.log.text) == "later");
}

TEST_CASE("the module table goes out in pages and a LogControl drives it")
{
    Captured captured;
    RecordingSink sink;
    const Session session(sink);
    REQUIRE(mark4::logPublishModules(&Captured::Send, &captured));

    const std::size_t total = mark4::logModuleCount();
    std::size_t listed = 0U;
    bool sawBaro = false;
    for (const mark4_Envelope &envelope : captured.envelopes)
    {
        REQUIRE(envelope.which_body == mark4_Envelope_log_modules_tag);
        const mark4_LogModules &page = envelope.body.log_modules;
        CHECK(page.total == total);
        CHECK(page.start_index == listed);
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
    }
    CHECK(listed == total);
    CHECK(sawBaro);
    CHECK(captured.envelopes.size() == (total + 7U) / 8U);

    mark4_LogControl control = mark4_LogControl_init_zero;
    control.which_request = mark4_LogControl_set_tag;
    control.request.set.module_id = BARO.id();
    control.request.set.level = mark4_LogLevel_TRACE;
    CHECK(mark4::logHandleControl(control));
    CHECK(BARO.level() == mark4::LogLevel::TRACE);
    CHECK(!mark4::logHandleControl(control)); // unchanged: nothing to republish
    control.request.set.module_id = 0xFFFFU;
    CHECK(!mark4::logHandleControl(control));
    control.request.set.module_id = BARO.id();
    // A level a newer peer knows and this build does not: a plain integer
    // on the wire (enum_intsize IS_32), refused rather than loaded.
    const int unknownLevel = 99;
    static_assert(sizeof(control.request.set.level) == sizeof(unknownLevel));
    std::memcpy(&control.request.set.level, &unknownLevel, sizeof(unknownLevel));
    CHECK(!mark4::logHandleControl(control));
    CHECK(BARO.level() == mark4::LogLevel::TRACE);
    control.which_request = mark4_LogControl_query_tag;
    control.request.query = true;
    CHECK(mark4::logHandleControl(control));

    captured.accept = false;
    CHECK(!mark4::logPublishModules(&Captured::Send, &captured));
}
