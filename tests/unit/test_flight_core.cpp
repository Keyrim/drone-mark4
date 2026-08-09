#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream

    /// Plausible static pressure carried by every helper frame. The default
    /// SensorFrame reads 0 Pa, a faulted sensor: a core fed with it must
    /// refuse to fly, so any test that flies has to provide a real baro.
    constexpr float HELPER_BARO_PA = 101325.0f;

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
            frame.baroPa = HELPER_BARO_PA;
            core.step(frame, actuators);
            timestamp += STEP_US;
        }
        return timestamp;
    }

    /// @brief Settles a fresh core at rest until it is ready to fly (baro
    ///        reference captured, estimators converged on a level attitude).
    /// @return timestamp to continue the stream from
    std::uint64_t settle(mark4::FlightCore &core, mark4::ActuatorFrame &actuators)
    {
        return feed(core, actuators, 0U, 60U, mark4::GRAVITY_MPS2);
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
    std::uint64_t timestamp = settle(core, actuators);

    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);

    // A kill (the RC fail-safe state included) returns to IDLE: releasing
    // the switch must never resume an armed or flying phase on its own.
    frame.rc.killSwitch = true;
    frame.rc.armSwitch = false;
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);

    // Releasing the kill alone stays IDLE until a deliberate rearm.
    frame.rc.killSwitch = false;
    frame.timestampUs = timestamp + 2U * STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
}

TEST_CASE("an out-of-order frame is ignored and the outputs hold")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t timestamp = settle(core, actuators);
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;

    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    const std::array<float, 4> held = actuators.motor;

    // Same timestamp, then an older one: both ignored, outputs held, even
    // though the frame content is violent enough to trip every cutoff.
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};
    core.step(frame, actuators);
    frame.timestampUs = timestamp - STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.staleFrameCount() == 2U);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(actuators.motor == held);

    // The stream resumes where it left off: fresh frames step normally.
    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
}

TEST_CASE("a frame carrying NaN or Inf is rejected as a whole")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t settled = settle(core, actuators);
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;

    frame.timestampUs = settled;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    const std::array<float, 4> held = actuators.motor;

    // One poisoned field per frame: each frame is ignored, the outputs
    // hold and stay numeric, the phase does not move.
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();
    mark4::SensorFrame bad = frame;
    std::uint64_t timestamp = settled + STEP_US;
    for (std::uint32_t poison = 0U; poison < 4U; ++poison)
    {
        bad = frame;
        bad.timestampUs = timestamp;
        switch (poison)
        {
            case 0U:
                bad.gyroRadS[1] = nan;
                break;
            case 1U:
                bad.accelMps2[2] = inf;
                break;
            case 2U:
                bad.baroPa = nan;
                break;
            default:
                bad.rc.throttle = inf;
                break;
        }
        core.step(bad, actuators);
        timestamp += STEP_US;
    }
    REQUIRE(core.invalidFrameCount() == 4U);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(actuators.motor == held);
    for (const float m : actuators.motor)
    {
        REQUIRE(std::isfinite(m));
    }

    // A clean frame resumes normal stepping (the poisoned timestamps were
    // never accepted, so this one is fresh).
    frame.timestampUs = settled + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    for (const float m : actuators.motor)
    {
        REQUIRE(std::isfinite(m));
    }
}

TEST_CASE("the kill switch wins even on a NaN frame")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    actuators.motor.fill(0.7f);

    frame.rc.killSwitch = true;
    frame.gyroRadS = {std::nanf(""), 0.0f, 0.0f};
    frame.timestampUs = 1000U;
    core.step(frame, actuators);

    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
}

TEST_CASE("a backwards timestamp during the recovery does not latch the cutoff")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    std::uint64_t timestamp = playThrow(core, actuators);
    while (core.flightPhase() == mark4::FlightPhase::BALLISTIC)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::RECOVERY);

    // An out-of-order frame used to wrap the unsigned elapsed-time math and
    // instantly latch CUTOFF mid-recovery. It must be ignored instead.
    mark4::SensorFrame frame;
    frame.timestampUs = timestamp - 500000U;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::RECOVERY);
}

TEST_CASE("a throttle below the arming threshold keeps the motors stopped")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t timestamp = settle(core, actuators);
    actuators.motor.fill(0.7f);

    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.0f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.timestampUs = timestamp;
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
    const std::uint64_t timestamp = settle(core, actuators);

    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f; // mid stick: hold the altitude
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    frame.timestampUs = timestamp + STEP_US;
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
    const std::uint64_t timestamp = settle(core, actuators);
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;

    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);

    // 10 g spike: immediate cutoff.
    frame.timestampUs = timestamp + STEP_US;
    frame.accelMps2 = {0.0f, 0.0f, 10.0f * mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Back to normal frames: still latched.
    frame.timestampUs = timestamp + 2U * STEP_US;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Stick down rearms, stick up flies again.
    frame.timestampUs = timestamp + 3U * STEP_US;
    frame.rc.throttle = 0.0f;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    frame.timestampUs = timestamp + 4U * STEP_US;
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
    const std::uint64_t timestamp = settle(core, actuators);
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};

    frame.timestampUs = timestamp;
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
    frame.baroPa = HELPER_BARO_PA;

    for (std::uint32_t i = 0U; i < 2500U; ++i)
    {
        frame.timestampUs = static_cast<std::uint64_t>(i) * 2000U;
        core.step(frame, actuators);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("a core with a dead baro never flies")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    // baroPa stays at the SensorFrame default of 0 Pa: a faulted sensor.

    // Stick raised and arm switch on, far past the capture window: with no
    // baro reference the state machine must refuse every mission.
    frame.rc.throttle = 0.5f;
    frame.rc.armSwitch = true;
    for (std::uint32_t i = 0U; i < 500U; ++i)
    {
        frame.timestampUs = static_cast<std::uint64_t>(i) * STEP_US;
        core.step(frame, actuators);
        REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    }
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("arming waits for the baro reference capture")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;

    // One frame short of the capture window: still not ready, still IDLE.
    std::uint64_t timestamp = 0U;
    for (std::uint32_t i = 0U; i + 1U < mark4::VerticalEstimator::REFERENCE_SAMPLES; ++i)
    {
        frame.timestampUs = timestamp;
        core.step(frame, actuators);
        timestamp += STEP_US;
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);

    // The frame completing the reference unlocks the arming.
    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
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
    const std::uint64_t timestamp = settle(core, actuators);
    frame.rc.killSwitch = false;
    frame.rc.throttle = 0.5f;
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;

    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    // Stick down with the arm switch still on: stays latched.
    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    frame.rc.throttle = 0.0f;
    frame.rc.armSwitch = true;
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    frame.rc.armSwitch = false;
    frame.timestampUs = timestamp + 2U * STEP_US;
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
