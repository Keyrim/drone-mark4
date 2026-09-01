#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "platform_stm32/motor_sink_dshot.hpp"

using namespace mark4;

TEST_CASE("dshot throttle maps a normalized command to the value space")
{
    // 0 stops the motor; the throttle band starts at 48 and tops out at 2047.
    REQUIRE(dshotThrottle(0.0f) == 0U);
    REQUIRE(dshotThrottle(-0.5f) == 0U); // killed stays stopped, never negative
    REQUIRE(dshotThrottle(1.0f) == 2047U);
    REQUIRE(dshotThrottle(2.0f) == 2047U); // clamped, not wrapped
    REQUIRE(dshotThrottle(0.5f) == 1048U); // 48 + round(0.5 * 1999)
}

TEST_CASE("dshot frame carries the value, telemetry bit and nibble CRC")
{
    // Known DShot vectors: zero is all zeros; 0x0606 is the classic 48 frame.
    REQUIRE(dshotFrame(0U, false) == 0x0000U);
    REQUIRE(dshotFrame(48U, false) == 0x0606U);
    // Full scale with the telemetry bit set is the all-ones frame.
    REQUIRE(dshotFrame(2047U, true) == 0xFFFFU);
}

TEST_CASE("dshot expansion writes per-bit high times MSB first")
{
    std::array<std::uint16_t, DSHOT_BITS> column{};

    dshotExpand(0xFFFFU, column.data(), 1U);
    for (const std::uint16_t slot : column)
    {
        REQUIRE(slot == DSHOT_T1H);
    }

    dshotExpand(0x0000U, column.data(), 1U);
    for (const std::uint16_t slot : column)
    {
        REQUIRE(slot == DSHOT_T0H);
    }

    // Only the most significant bit is set: the first slot is a 1, the rest 0.
    dshotExpand(0x8000U, column.data(), 1U);
    REQUIRE(column[0] == DSHOT_T1H);
    for (std::size_t i = 1U; i < DSHOT_BITS; ++i)
    {
        REQUIRE(column[i] == DSHOT_T0H);
    }
}
