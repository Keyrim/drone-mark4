/// @file
/// @brief The stick mapping: deadband, ranges, and the one place where the
///        pilot's convention (right, forward, clockwise) meets the body
///        frame (x forward, y left, z up).

#include <array>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/stick_mapper.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr float EPS = 1e-5f;

    /// @param roll roll stick
    /// @param pitch pitch stick
    /// @param yaw yaw stick
    /// @return an RC state flying with those sticks
    mark4::RcInput sticks(float roll, float pitch, float yaw)
    {
        mark4::RcInput rc;
        rc.killSwitch = false;
        rc.armSwitch = true;
        rc.throttle = 0.5f;
        rc.roll = roll;
        rc.pitch = pitch;
        rc.yaw = yaw;
        return rc;
    }

    /// @param angleRad rotation about world z
    /// @return the attitude of a level drone with that heading
    mark4::Quaternion heading(float angleRad)
    {
        return {std::cos(angleRad / 2.0f), 0.0f, 0.0f, std::sin(angleRad / 2.0f)};
    }
} // namespace

TEST_CASE("the stick deflection is zero inside the deadband, then continuous and full range")
{
    const mark4::StickMapper mapper;
    const float deadband = mapper.deadband();

    REQUIRE(mapper.deflection(0.0f) == 0.0f);
    REQUIRE(mapper.deflection(deadband) == 0.0f);
    REQUIRE(mapper.deflection(-deadband) == 0.0f);

    // A hair outside the band is a hair of deflection, never a step.
    const float justOut = mapper.deflection(deadband + 0.001f);
    REQUIRE(justOut > 0.0f);
    REQUIRE(justOut < 0.01f);

    // Full stick reaches exactly one, both ways, and beyond the range it
    // stays there: a controller reporting 1.2 is a full stick.
    REQUIRE(mapper.deflection(1.0f) == 1.0f);
    REQUIRE(mapper.deflection(-1.0f) == -1.0f);
    REQUIRE(mapper.deflection(1.2f) == 1.0f);
    REQUIRE(mapper.deflection(-7.0f) == -1.0f);

    // Monotone and antisymmetric in between.
    float previous = -1.0f;
    for (std::uint32_t i = 0U; i <= 200U; ++i)
    {
        const float stick = static_cast<float>(i) / 100.0f - 1.0f;
        const float value = mapper.deflection(stick);
        REQUIRE(value >= previous);
        REQUIRE(std::fabs(value + mapper.deflection(-stick)) < EPS);
        previous = value;
    }
}

TEST_CASE("the manual sticks command body rates in the body frame")
{
    const mark4::StickMapper mapper;

    // Released: nothing on any axis.
    std::array<float, 3> rates = mapper.rateSetpointRadS(sticks(0.0f, 0.0f, 0.0f));
    REQUIRE(rates == std::array<float, 3>{0.0f, 0.0f, 0.0f});

    // Roll right: a positive rate about x (y points left, so a positive
    // rotation about x lifts the left side).
    rates = mapper.rateSetpointRadS(sticks(1.0f, 0.0f, 0.0f));
    REQUIRE(rates[0] == mark4::StickMapper::DEFAULT_RATE_ROLL_PITCH_RADS);
    REQUIRE(rates[1] == 0.0f);
    REQUIRE(rates[2] == 0.0f);

    // Nose down: a positive rate about y.
    rates = mapper.rateSetpointRadS(sticks(0.0f, 1.0f, 0.0f));
    REQUIRE(rates[0] == 0.0f);
    REQUIRE(rates[1] == mark4::StickMapper::DEFAULT_RATE_ROLL_PITCH_RADS);

    // Clockwise seen from above: a negative rate about z, which points up.
    rates = mapper.rateSetpointRadS(sticks(0.0f, 0.0f, 1.0f));
    REQUIRE(rates[2] == -mark4::StickMapper::DEFAULT_RATE_YAW_RADS);
    REQUIRE(mapper.yawRateRadS(sticks(0.0f, 0.0f, -0.5f)) > 0.0f);

    // Half stick past the deadband is half the range, on the ramp.
    rates = mapper.rateSetpointRadS(sticks(-0.5f, 0.0f, 0.0f));
    REQUIRE(rates[0] < 0.0f);
    REQUIRE(rates[0] > -mark4::StickMapper::DEFAULT_RATE_ROLL_PITCH_RADS);
}

TEST_CASE("the stick ranges are what the setters say")
{
    mark4::StickMapper mapper;
    mapper.setRateRollPitch(1.0f);
    mapper.setRateYaw(2.0f);
    mapper.setDeadband(0.0f);

    const std::array<float, 3> rates = mapper.rateSetpointRadS(sticks(0.5f, -0.25f, 1.0f));
    REQUIRE(std::fabs(rates[0] - 0.5f) < EPS);
    REQUIRE(std::fabs(rates[1] + 0.25f) < EPS);
    REQUIRE(std::fabs(rates[2] + 2.0f) < EPS);

    // A zero range holds an axis still whatever the stick does.
    mapper.setRateRollPitch(0.0f);
    REQUIRE(mapper.rateSetpointRadS(sticks(1.0f, 1.0f, 0.0f))[0] == 0.0f);
    REQUIRE(mapper.rateSetpointRadS(sticks(1.0f, 1.0f, 0.0f))[1] == 0.0f);
}

TEST_CASE("released sticks in the leveling mode ask for straight up")
{
    const mark4::StickMapper mapper;
    for (const float yaw : {0.0f, 1.0f, -2.5f})
    {
        const std::array<float, 3> up =
            mapper.desiredUpWorld(sticks(0.0f, 0.0f, 0.0f), heading(yaw));
        REQUIRE(std::fabs(up[0]) < EPS);
        REQUIRE(std::fabs(up[1]) < EPS);
        REQUIRE(std::fabs(up[2] - 1.0f) < EPS);
    }
}

TEST_CASE("the leveling sticks lean the thrust in the drone's own heading")
{
    const mark4::StickMapper mapper;
    const float tanMax = std::tan(mark4::StickMapper::DEFAULT_TILT_MAX_RAD);

    // Nose down at a zero heading: the up axis leans toward world +x, and
    // the full stick reaches exactly the maximum tilt.
    std::array<float, 3> up = mapper.desiredUpWorld(sticks(0.0f, 1.0f, 0.0f), heading(0.0f));
    REQUIRE(up[0] > 0.0f);
    REQUIRE(std::fabs(up[1]) < EPS);
    REQUIRE(std::fabs(up[0] / up[2] - tanMax) < EPS);

    // Roll right: toward world -y (the body y axis points left).
    up = mapper.desiredUpWorld(sticks(1.0f, 0.0f, 0.0f), heading(0.0f));
    REQUIRE(std::fabs(up[0]) < EPS);
    REQUIRE(up[1] < 0.0f);

    // Nose down with the nose pointing at world +y (a quarter turn to the
    // left): forward is now world +y, so that is where the lean goes.
    const float quarter = 3.14159265f / 2.0f;
    up = mapper.desiredUpWorld(sticks(0.0f, 1.0f, 0.0f), heading(quarter));
    REQUIRE(std::fabs(up[0]) < 1e-4f);
    REQUIRE(up[1] > 0.0f);

    // And roll right with that heading leans toward world +x.
    up = mapper.desiredUpWorld(sticks(1.0f, 0.0f, 0.0f), heading(quarter));
    REQUIRE(up[0] > 0.0f);
    REQUIRE(std::fabs(up[1]) < 1e-4f);
}

TEST_CASE("the leveling lean is a unit vector bounded by the maximum tilt")
{
    mark4::StickMapper mapper;

    // A diagonal full stick leans no further than a straight one.
    const std::array<float, 3> diagonal =
        mapper.desiredUpWorld(sticks(1.0f, 1.0f, 0.0f), heading(0.3f));
    const float norm = std::sqrt(diagonal[0] * diagonal[0] + diagonal[1] * diagonal[1] +
                                 diagonal[2] * diagonal[2]);
    REQUIRE(std::fabs(norm - 1.0f) < EPS);
    const float tilt = std::acos(diagonal[2]);
    REQUIRE(std::fabs(tilt - mark4::StickMapper::DEFAULT_TILT_MAX_RAD) < 1e-4f);

    // The bound follows the setter.
    mapper.setTiltMax(0.1f);
    const std::array<float, 3> gentle =
        mapper.desiredUpWorld(sticks(1.0f, 1.0f, 0.0f), heading(0.3f));
    REQUIRE(std::fabs(std::acos(gentle[2]) - 0.1f) < 1e-4f);
    REQUIRE(mapper.tiltMaxRad() == 0.1f);
}

TEST_CASE("a vertical drone has no heading and leans along the world axes")
{
    const mark4::StickMapper mapper;
    // Nose straight up: a quarter turn about y that brings the body x axis
    // onto world z. The heading is undefined and the mapping must still
    // return a finite unit vector.
    const float half = -3.14159265f / 4.0f;
    const mark4::Quaternion noseUp{std::cos(half), 0.0f, std::sin(half), 0.0f};
    const std::array<float, 3> up = mapper.desiredUpWorld(sticks(0.5f, 0.5f, 0.0f), noseUp);
    const float norm = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    REQUIRE(std::isfinite(norm));
    REQUIRE(std::fabs(norm - 1.0f) < EPS);
}
