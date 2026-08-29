#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "platform_common/rc_tracker.hpp"
#include "protocol/envelope.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 1000000U;
    constexpr float TEST_THROTTLE = 0.625f;

    /// @param kill true = engaged
    /// @param arm true = armed
    /// @param throttle normalized throttle
    /// @param mode piloting mode
    /// @return one Rc message
    mark4_Rc makeRc(bool kill, bool arm, float throttle, mark4_RcMode mode = mark4_RcMode_RC_MANUAL)
    {
        mark4_Rc rc = mark4_Rc_init_zero;
        rc.kill = kill;
        rc.arm = arm;
        rc.throttle = throttle;
        rc.mode = mode;
        return rc;
    }
} // namespace

TEST_CASE("before any rc message the frame reverts to the safe state")
{
    mark4::RcTracker tracker;

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.throttle = TEST_THROTTLE;

    REQUIRE(tracker.failsafeActive(frame.timestampUs));
    tracker.graft(frame);

    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(frame.rc.armSwitch == false);
    REQUIRE(frame.rc.throttle == 0.0f);
    REQUIRE(tracker.rcPacketCount() == 0U);
}

TEST_CASE("an rc message reaches the frame")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, true, TEST_THROTTLE), T0_US);
    REQUIRE(tracker.rcPacketCount() == 1U);

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    REQUIRE(!tracker.failsafeActive(frame.timestampUs));
    tracker.graft(frame);

    REQUIRE(frame.rc.killSwitch == false);
    REQUIRE(frame.rc.armSwitch == true);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("silence on the rc uplink reverts to kill within the timeout")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, true, TEST_THROTTLE), T0_US);

    // The whole timeout window still holds the last known state.
    mark4::SensorFrame edge;
    edge.timestampUs = T0_US + mark4::RcTracker::RC_TIMEOUT_US;
    REQUIRE(!tracker.failsafeActive(edge.timestampUs));
    tracker.graft(edge);
    REQUIRE(edge.rc.killSwitch == false);
    REQUIRE(edge.rc.armSwitch == true);
    REQUIRE(edge.rc.throttle == TEST_THROTTLE);

    // One microsecond past it, the safe state wins.
    mark4::SensorFrame lost;
    lost.timestampUs = T0_US + mark4::RcTracker::RC_TIMEOUT_US + 1U;
    REQUIRE(tracker.failsafeActive(lost.timestampUs));
    tracker.graft(lost);
    REQUIRE(lost.rc.killSwitch == true);
    REQUIRE(lost.rc.armSwitch == false);
    REQUIRE(lost.rc.throttle == 0.0f);
}

TEST_CASE("a fresh rc message recovers from the fail-safe")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, true, TEST_THROTTLE), T0_US);

    const std::uint64_t lateUs = T0_US + 2U * mark4::RcTracker::RC_TIMEOUT_US;
    REQUIRE(tracker.failsafeActive(lateUs));

    tracker.onRc(makeRc(false, true, TEST_THROTTLE), lateUs);
    REQUIRE(!tracker.failsafeActive(lateUs));

    mark4::SensorFrame frame;
    frame.timestampUs = lateUs;
    tracker.graft(frame);
    REQUIRE(frame.rc.killSwitch == false);
    REQUIRE(frame.rc.armSwitch == true);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("the last rc message of a burst wins")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, false, 0.25f), T0_US);
    tracker.onRc(makeRc(false, true, 0.5f), T0_US);
    tracker.onRc(makeRc(true, false, TEST_THROTTLE), T0_US);
    REQUIRE(tracker.rcPacketCount() == 3U);

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    tracker.graft(frame);
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(frame.rc.armSwitch == false);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("the piloting mode of an rc message reaches the frame")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, true, TEST_THROTTLE, mark4_RcMode_RC_ALTITUDE_AUTO), T0_US);

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    tracker.graft(frame);
    REQUIRE(frame.rc.mode == mark4::PilotMode::ALTITUDE_AUTO);

    // Back to manual: the mode is a level like the switches are.
    tracker.onRc(makeRc(false, true, TEST_THROTTLE, mark4_RcMode_RC_MANUAL), T0_US);
    tracker.graft(frame);
    REQUIRE(frame.rc.mode == mark4::PilotMode::MANUAL);
}

TEST_CASE("an unknown mode decodes to the safest mode")
{
    mark4::RcTracker tracker;
    // A ground station from the future selecting a mode this build does not
    // know: the message is still valid RC, only the mode degrades. The cast
    // is exactly what the decoder produces for a value outside the enum.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto unknown = static_cast<mark4_RcMode>(0x7F);
    tracker.onRc(makeRc(false, true, TEST_THROTTLE, unknown), T0_US);
    REQUIRE(tracker.rcPacketCount() == 1U);

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    tracker.graft(frame);
    REQUIRE(frame.rc.mode == mark4::PilotMode::MANUAL);
    REQUIRE(frame.rc.armSwitch == true);
}

TEST_CASE("the fail-safe reverts the piloting mode too")
{
    mark4::RcTracker tracker;
    tracker.onRc(makeRc(false, true, TEST_THROTTLE, mark4_RcMode_RC_ALTITUDE_AUTO), T0_US);

    mark4::SensorFrame lost;
    lost.timestampUs = T0_US + mark4::RcTracker::RC_TIMEOUT_US + 1U;
    REQUIRE(tracker.failsafeActive(lost.timestampUs));
    tracker.graft(lost);
    REQUIRE(lost.rc.mode == mark4::PilotMode::MANUAL);
    REQUIRE(lost.rc.killSwitch == true);
}
