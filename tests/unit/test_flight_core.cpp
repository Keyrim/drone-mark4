#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream

    /// @brief Feeds frames with the given accel on z, and optionally on x.
    /// @return timestamp to continue the stream from
    std::uint64_t feed(mark4::FlightCore &core,
                       mark4::ActuatorFrame &actuators,
                       std::uint64_t fromUs,
                       std::uint32_t steps,
                       float accelZ,
                       bool armSwitch = false,
                       float accelX = 0.0f)
    {
        std::uint64_t timestamp = fromUs;
        for (std::uint32_t i = 0U; i < steps; ++i)
        {
            mark4::SensorFrame frame;
            frame.timestampUs = timestamp;
            frame.rc.killSwitch = false;
            frame.rc.armSwitch = armSwitch;
            frame.accelMps2 = {accelX, 0.0f, accelZ};
            core.step(frame, actuators);
            timestamp += STEP_US;
        }
        return timestamp;
    }

    /// @brief Plays rest, then a hand thrust, up to the confirmed detection.
    /// @return timestamp to continue the stream from, in free fall
    std::uint64_t playThrow(mark4::FlightCore &core, mark4::ActuatorFrame &actuators)
    {
        // Rest settles the estimators and the baro reference.
        std::uint64_t timestamp = feed(core, actuators, 0U, 200U, mark4::GRAVITY_MPS2, true);
        // 100 ms of 5 g thrust leaves at about 4 m/s.
        timestamp = feed(core, actuators, timestamp, 50U, 5.0f * mark4::GRAVITY_MPS2, true);
        // Free fall until the detector confirms the throw.
        while (core.flightPhase() == mark4::FlightPhase::ARMED)
        {
            timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
        }
        return timestamp;
    }
} // namespace

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
    frame.timestampUs = 123456U;
    core.step(frame, actuators);

    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
    // Outputs are stamped with the input they answer, kill switch included.
    REQUIRE(actuators.timestampUs == frame.timestampUs);
}

TEST_CASE("the kill switch ends the mission and disarms the state machine")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;

    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.timestampUs = 1000U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);

    // A kill (the RC fail-safe state included) returns to IDLE: releasing
    // the switch must never resume an armed or flying phase on its own.
    frame.rc.killSwitch = true;
    frame.rc.armSwitch = false;
    frame.timestampUs = 3000U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);

    // Releasing the kill alone stays IDLE until a deliberate rearm.
    frame.rc.killSwitch = false;
    frame.timestampUs = 5000U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
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

    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
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
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
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
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);

    // 10 g spike: immediate cutoff.
    frame.timestampUs = 2000U;
    frame.accelMps2 = {0.0f, 0.0f, 10.0f * mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Back to normal frames: still latched.
    frame.timestampUs = 4000U;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Stick down rearms, stick up flies again.
    frame.timestampUs = 6000U;
    frame.rc.throttle = 0.0f;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    frame.timestampUs = 8000U;
    frame.rc.throttle = 0.5f;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
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
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
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
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("the arm switch gates the throw flight and disarming aborts it")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    // A throw with the arm switch off never leaves IDLE.
    std::uint64_t timestamp = feed(core, actuators, 0U, 200U, mark4::GRAVITY_MPS2);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    timestamp = feed(core, actuators, timestamp, 50U, 5.0f * mark4::GRAVITY_MPS2);
    feed(core, actuators, timestamp, 100U, 0.0f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Arm, then release the switch mid ballistic: straight back to IDLE.
    mark4::FlightCore armed;
    timestamp = playThrow(armed, actuators);
    REQUIRE(armed.flightPhase() == mark4::FlightPhase::BALLISTIC);
    feed(armed, actuators, timestamp, 1U, 0.0f, false);
    REQUIRE(armed.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("a detected throw spins up ahead of the apex and recovers into a hover")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    std::uint64_t timestamp = playThrow(core, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::BALLISTIC);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Coast in free fall: the motors must stay off until the spin-up instant.
    std::uint64_t firstSpinUs = 0U;
    for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
        if (firstSpinUs == 0U && actuators.motor[0] > 0.0f)
        {
            firstSpinUs = timestamp - STEP_US;
        }
    }

    // Spin-up happened, before the predicted apex by about the lead time.
    const std::uint64_t apexUs = core.throwDetector().apexTimestampUs();
    REQUIRE(firstSpinUs > 0U);
    REQUIRE(firstSpinUs + mark4::FlightCore::SPINUP_LEAD_US >= apexUs);
    REQUIRE(firstSpinUs + mark4::FlightCore::SPINUP_LEAD_US < apexUs + 2U * STEP_US);

    // Thrown level, the recovery hands over to the altitude hold quickly.
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
    REQUIRE(actuators.motor[0] > 0.0f);

    // The pilot takes over by raising the stick.
    mark4::SensorFrame frame;
    frame.timestampUs = timestamp;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
}

TEST_CASE("a ballistic phase ending on the ground returns to armed without spinning")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    std::uint64_t timestamp = playThrow(core, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::BALLISTIC);

    // Caught by hand right after the detection: sustained 1 g, no impact
    // spike. The detector drops back to idle before the spin-up instant.
    feed(core, actuators, timestamp, 100U, mark4::GRAVITY_MPS2, true);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("leaving the cutoff needs both the stick down and the arm switch off")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

    frame.timestampUs = 0U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    // Stick down with the arm switch still on: stays latched.
    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    frame.rc.throttle = 0.0f;
    frame.rc.armSwitch = true;
    frame.timestampUs = 2000U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    frame.rc.armSwitch = false;
    frame.timestampUs = 4000U;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
}

TEST_CASE("the post-throw hover leans against the momentum then levels for good")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    // A throw with a forward component: 100 ms at 5 g up plus 2 g forward
    // leaves about 2 m/s of horizontal momentum in the dead reckoning.
    std::uint64_t timestamp = feed(core, actuators, 0U, 200U, mark4::GRAVITY_MPS2, true);
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     50U,
                     5.0f * mark4::GRAVITY_MPS2,
                     true,
                     2.0f * mark4::GRAVITY_MPS2);
    while (core.flightPhase() == mark4::FlightPhase::ARMED)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);

    // Inside the braking window: the front and rear motors split to pitch
    // the nose up against the forward drift.
    timestamp = feed(core, actuators, timestamp, 3U, mark4::GRAVITY_MPS2, true);
    const float braking = std::fabs(actuators.motor[1] - actuators.motor[0]);
    REQUIRE(braking > 0.01f);

    // One frame far past the window: level again, the lean is gone.
    feed(core, actuators, timestamp + 5000000U, 1U, mark4::GRAVITY_MPS2, true);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
    const float leveled = std::fabs(actuators.motor[1] - actuators.motor[0]);
    REQUIRE(leveled < 0.2f * braking);
}

TEST_CASE("the braking is one-shot: a reborn estimate never re-leans the drone")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    // A purely vertical throw: no momentum, the braking is spent on arrival.
    std::uint64_t timestamp = playThrow(core, actuators);
    for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
    timestamp = feed(core, actuators, timestamp, 5U, mark4::GRAVITY_MPS2, true);

    // Rebuild a dead reckoned velocity: 100 ms of 2 g sideways, outside the
    // Mahony gate so the attitude estimate stays level (the phantom case).
    timestamp = feed(
        core, actuators, timestamp, 50U, mark4::GRAVITY_MPS2, true, 2.0f * mark4::GRAVITY_MPS2);

    // Level frames again: the braking must not restart on the phantom.
    feed(core, actuators, timestamp, 5U, mark4::GRAVITY_MPS2, true);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
    REQUIRE(std::fabs(actuators.motor[1] - actuators.motor[0]) < 0.003f);
}
