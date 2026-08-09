#include <array>
#include <cstdio>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/blackbox.hpp"
#include "flight_core/types.hpp"
#include "platform_replay/sensor_source_replay.hpp"
#include "platform_sim/log_sink_file.hpp"

namespace
{
    constexpr const char *TEST_LOG_PATH = "test_replay.m4bb";

    /// @return a sensor frame whose fields all depend on the seed
    mark4::SensorFrame makeFrame(std::uint64_t seed)
    {
        mark4::SensorFrame frame;
        frame.timestampUs = 1000U * seed;
        frame.gyroRadS = {0.1f * static_cast<float>(seed), -0.2f, 0.3f};
        frame.accelMps2 = {0.0f, 0.5f * static_cast<float>(seed), 9.81f};
        frame.baroPa = 101325.0f + static_cast<float>(seed);
        frame.rc.killSwitch = (seed % 2U) == 0U;
        frame.rc.throttle = 0.1f * static_cast<float>(seed);
        frame.rc.armSwitch = (seed % 3U) == 0U;
        return frame;
    }
} // namespace

TEST_CASE("a recorded blackbox replays into identical sensor frames")
{
    constexpr std::uint64_t FRAME_COUNT = 3U;
    {
        mark4::LogSinkFile sink(TEST_LOG_PATH);
        REQUIRE(sink.init());
        mark4::Blackbox blackbox(sink);
        for (std::uint64_t seed = 1U; seed <= FRAME_COUNT; ++seed)
        {
            const mark4::ActuatorFrame actuators;
            blackbox.record(makeFrame(seed), actuators);
        }
    } // the sink destructor closes and flushes the file

    mark4::SensorSourceReplay source(mark4::SensorSourceReplay::SPEED_MAX);
    REQUIRE(source.init(TEST_LOG_PATH));

    for (std::uint64_t seed = 1U; seed <= FRAME_COUNT; ++seed)
    {
        const mark4::SensorFrame expected = makeFrame(seed);
        mark4::SensorFrame replayed;
        REQUIRE(source.waitFrame(replayed) == mark4::FrameWait::FRAME);
        REQUIRE(replayed.timestampUs == expected.timestampUs);
        REQUIRE(replayed.gyroRadS == expected.gyroRadS);
        REQUIRE(replayed.accelMps2 == expected.accelMps2);
        REQUIRE(replayed.baroPa == expected.baroPa);
        REQUIRE(replayed.rc.killSwitch == expected.rc.killSwitch);
        REQUIRE(replayed.rc.throttle == expected.rc.throttle);
        REQUIRE(replayed.rc.armSwitch == expected.rc.armSwitch);
    }

    mark4::SensorFrame extra;
    // REQUIRE(!x) rather than REQUIRE_FALSE: the FalseTest flag combination
    // trips clang-analyzer's enum-cast check inside Catch2's own headers.
    REQUIRE(source.waitFrame(extra) == mark4::FrameWait::EXHAUSTED);

    REQUIRE(std::remove(TEST_LOG_PATH) == 0);
}

TEST_CASE("a missing replay file fails init without producing frames")
{
    mark4::SensorSourceReplay source(mark4::SensorSourceReplay::SPEED_MAX);
    REQUIRE(!source.init("does_not_exist.m4bb"));

    mark4::SensorFrame frame;
    REQUIRE(source.waitFrame(frame) == mark4::FrameWait::EXHAUSTED);
}
