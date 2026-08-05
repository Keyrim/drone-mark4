#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"

TEST_CASE("step increments the step counter")
{
    mark4::FlightCore core;
    const mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;

    REQUIRE(core.stepCount() == 0U);
    core.step(frame, actuators);
    core.step(frame, actuators);
    REQUIRE(core.stepCount() == 2U);
}

TEST_CASE("kill switch forces all motors to zero")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    actuators.motor.fill(0.7f);

    frame.rc.killSwitch = true;
    frame.rc.throttle = 0.9f;
    core.step(frame, actuators);

    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
}

TEST_CASE("a throttle below the arming threshold keeps the motors stopped")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    actuators.motor.fill(0.7f);

    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.0f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);

    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
}

TEST_CASE("a raised throttle drives the motors through the hover stack")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;

    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f; // mid stick: hold the altitude
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.timestampUs = 0U;
    core.step(frame, actuators);
    frame.timestampUs = 2000U;
    core.step(frame, actuators);

    // Level, at rest, mid stick: every motor sits near the hover collective.
    for (const float m : actuators.motor)
    {
        REQUIRE(m > 0.3f);
        REQUIRE(m < 0.8f);
    }
}

TEST_CASE("an impact cuts the motors and latches until the stick is lowered")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

    frame.timestampUs = 0U;
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::ARMED);

    // 10 g spike: immediate cutoff.
    frame.timestampUs = 2000U;
    frame.accelMps2 = {0.0f, 0.0f, 10.0f * mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::CUTOFF);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Back to normal frames: still latched.
    frame.timestampUs = 4000U;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Stick down rearms, stick up flies again.
    frame.timestampUs = 6000U;
    frame.rc.throttle = 0.0f;
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::DISARMED);
    frame.timestampUs = 8000U;
    frame.rc.throttle = 0.5f;
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::ARMED);
    REQUIRE(actuators.motor[0] > 0.0f);
}

TEST_CASE("saturated rates cut the motors")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};

    frame.timestampUs = 0U;
    core.step(frame, actuators);
    REQUIRE(core.armState() == mark4::ArmState::CUTOFF);
}

TEST_CASE("a sustained unrecoverable tilt cuts the motors")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    // Gravity almost sideways in the body frame: the estimator converges
    // toward a tilt far beyond what the hover stack can recover.
    frame.accelMps2 = {0.0f, 0.985f * mark4::GRAVITY_MPS2, 0.174f * mark4::GRAVITY_MPS2};

    for (std::uint32_t i = 0U; i < 2500U; ++i)
    {
        frame.timestampUs = static_cast<std::uint64_t>(i) * 2000U;
        core.step(frame, actuators);
    }
    REQUIRE(core.armState() == mark4::ArmState::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);
}
