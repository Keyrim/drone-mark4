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

TEST_CASE("throttle is copied to motors when kill switch is released")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;

    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.25f;
    core.step(frame, actuators);

    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.25f);
    }
}
