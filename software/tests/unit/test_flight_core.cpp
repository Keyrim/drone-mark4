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
    ///        The default mode and throttle are the altitude-auto interlock
    ///        (mode selected, stick centered): with the arm switch off that
    ///        is a drone sitting on the ground, and flipping the switch on is
    ///        all it takes to arm for a throw.
    /// @return timestamp to continue the stream from
    std::uint64_t feed(mark4::FlightCore &core,
                       mark4::ActuatorFrame &actuators,
                       std::uint64_t fromUs,
                       std::uint32_t steps,
                       float accelZ,
                       bool armSwitch = false,
                       float accelX = 0.0f,
                       mark4::PilotMode mode = mark4::PilotMode::ALTITUDE_AUTO,
                       float throttle = 0.5f,
                       bool imuValid = true,
                       bool baroValid = true)
    {
        std::uint64_t timestamp = fromUs;
        for (std::uint32_t i = 0U; i < steps; ++i)
        {
            mark4::SensorFrame frame;
            frame.timestampUs = timestamp;
            frame.rc.killSwitch = false;
            frame.rc.armSwitch = armSwitch;
            frame.rc.mode = mode;
            frame.rc.throttle = throttle;
            frame.accelMps2 = {accelX, 0.0f, accelZ};
            frame.baroPa = HELPER_BARO_PA;
            frame.imuValid = imuValid;
            frame.baroValid = baroValid;
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

    /// @brief Settles a fresh core with the stick down in direct-thrust mode,
    ///        then flips the arm switch: the shortest path to a phase where
    ///        the motors may run, for tests about something else entirely.
    /// @return timestamp to continue the stream from, core in MANUAL
    std::uint64_t enterManual(mark4::FlightCore &core, mark4::ActuatorFrame &actuators)
    {
        std::uint64_t timestamp = feed(core,
                                       actuators,
                                       0U,
                                       60U,
                                       mark4::GRAVITY_MPS2,
                                       false,
                                       0.0f,
                                       mark4::PilotMode::MANUAL,
                                       0.0f);
        return feed(core,
                    actuators,
                    timestamp,
                    1U,
                    mark4::GRAVITY_MPS2,
                    true,
                    0.0f,
                    mark4::PilotMode::MANUAL,
                    0.0f);
    }

    /// @brief Fills a frame with the RC state that keeps a direct-thrust
    ///        flight going: armed, manual, at the given stick position.
    /// @param[in,out] frame frame to fill
    /// @param throttle stick position
    void manualRc(mark4::SensorFrame &frame, float throttle)
    {
        frame.rc.killSwitch = false;
        frame.rc.armSwitch = true;
        frame.rc.mode = mark4::PilotMode::MANUAL;
        frame.rc.throttle = throttle;
        frame.baroPa = HELPER_BARO_PA;
        frame.imuValid = true;
        frame.baroValid = true;
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
    frame.imuValid = true;
    frame.baroValid = true;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
    frame.rc.throttle = 0.5f;
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
    const std::uint64_t timestamp = enterManual(core, actuators);
    manualRc(frame, 0.5f);
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

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
    const std::uint64_t settled = enterManual(core, actuators);
    manualRc(frame, 0.5f);
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

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
    for (std::uint32_t poison = 0U; poison < 7U; ++poison)
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
            case 3U:
                bad.rc.roll = nan;
                break;
            case 4U:
                bad.rc.pitch = inf;
                break;
            case 5U:
                bad.rc.yaw = nan;
                break;
            default:
                bad.rc.throttle = inf;
                break;
        }
        core.step(bad, actuators);
        timestamp += STEP_US;
    }
    REQUIRE(core.invalidFrameCount() == 7U);
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
    frame.imuValid = true;
    frame.baroValid = true;
    frame.timestampUs = timestamp;
    core.step(frame, actuators);

    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Armed in direct-thrust mode, the same stick position is a legal
    // flight phase - and still exactly zero thrust, which is the point of
    // entering that mode with the stick at the bottom.
    actuators.motor.fill(0.7f);
    frame.rc.armSwitch = true;
    frame.rc.mode = mark4::PilotMode::MANUAL;
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
}

TEST_CASE("taking a hover over drives the motors through the altitude stack")
{
    // Altitude-auto piloted flight is only ever reached by taking over a
    // hover: it is not a takeoff mode. Fly a throw, then grab it.
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = playThrow(core, actuators);
    for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
    // One centered frame arms the takeover, the way a pilot recentres before
    // grabbing the drone.
    timestamp = feed(core, actuators, timestamp, 1U, mark4::GRAVITY_MPS2, true);

    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
    frame.rc.throttle = 0.6f; // off centre: the takeover gesture
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;
    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ALTITUDE_AUTO);
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);

    // Level, near rest: every motor sits around the hover collective.
    for (const float m : actuators.motor)
    {
        REQUIRE(m > 0.3f);
        REQUIRE(m < 0.8f);
    }
}

TEST_CASE("an impact cuts the motors and latches until the arm switch is released")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t timestamp = enterManual(core, actuators);
    manualRc(frame, 0.5f);
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

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

    // Back to normal frames, stick lowered: the cutoff is latched on the arm
    // switch alone now, so lowering the stick changes nothing.
    frame.timestampUs = timestamp + 2U * STEP_US;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.rc.throttle = 0.0f;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Releasing the arm switch rearms, flipping it back flies again: the
    // per-mode interlock (stick down, manual) is rechecked on the way out.
    frame.timestampUs = timestamp + 3U * STEP_US;
    frame.rc.armSwitch = false;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    frame.timestampUs = timestamp + 4U * STEP_US;
    frame.rc.armSwitch = true;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    frame.timestampUs = timestamp + 5U * STEP_US;
    frame.rc.throttle = 0.5f;
    core.step(frame, actuators);
    REQUIRE(actuators.motor[0] > 0.0f);
}

TEST_CASE("saturated rates cut the motors")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t timestamp = enterManual(core, actuators);
    manualRc(frame, 0.0f);
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
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
    frame.rc.armSwitch = true;
    frame.rc.throttle = 0.0f;
    // Gravity almost sideways in the body frame: the estimator converges
    // toward a tilt far beyond what the hover stack can recover.
    frame.accelMps2 = {0.0f, 0.985f * mark4::GRAVITY_MPS2, 0.174f * mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;

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

    // Stick centered and arm switch on, far past the capture window: with no
    // baro reference the state machine must refuse every mission.
    frame.rc.throttle = 0.5f;
    frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
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
    frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
    frame.rc.throttle = 0.5f;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;

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

    // Recentre once inside the hover, then move off centre: that gesture,
    // not a stick position, is what hands the drone to the pilot.
    timestamp = feed(core, actuators, timestamp, 1U, mark4::GRAVITY_MPS2, true);
    mark4::SensorFrame frame;
    frame.timestampUs = timestamp;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
    frame.rc.throttle = 0.7f;
    frame.imuValid = true;
    frame.baroValid = true;
    frame.baroPa = HELPER_BARO_PA;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ALTITUDE_AUTO);
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

TEST_CASE("leaving the cutoff needs the arm switch released")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    const std::uint64_t timestamp = enterManual(core, actuators);
    manualRc(frame, 0.0f);
    frame.gyroRadS = {70.0f, 0.0f, 0.0f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};

    frame.timestampUs = timestamp;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    // Any stick position, arm switch still on: stays latched.
    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    frame.rc.throttle = 0.5f;
    frame.timestampUs = timestamp + 2U * STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);

    frame.rc.armSwitch = false;
    frame.timestampUs = timestamp + 3U * STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
}

TEST_CASE("a kill mid-recovery cuts the motors and ends the mission")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    std::uint64_t timestamp = playThrow(core, actuators);
    while (core.flightPhase() == mark4::FlightPhase::BALLISTIC)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::RECOVERY);
    REQUIRE(actuators.motor[0] > 0.0f);

    mark4::SensorFrame frame;
    frame.timestampUs = timestamp;
    frame.rc.killSwitch = true;
    frame.rc.armSwitch = true;
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Releasing the kill with the arm switch off must not resume anything.
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = false;
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("a kill mid-hover cuts the motors and ends the mission")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    std::uint64_t timestamp = playThrow(core, actuators);
    for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
    {
        timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);

    mark4::SensorFrame frame;
    frame.timestampUs = timestamp;
    frame.rc.killSwitch = true;
    frame.rc.armSwitch = true;
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }
}

TEST_CASE("a kill clears the tilt streak: a rearm needs a fresh confirmation")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = enterManual(core, actuators);
    manualRc(frame, 0.0f);
    // Gravity almost sideways: the estimate converges beyond the tilt cutoff.
    frame.accelMps2 = {0.0f, 0.985f * mark4::GRAVITY_MPS2, 0.174f * mark4::GRAVITY_MPS2};

    auto stepOnce = [&]() {
        frame.timestampUs = timestamp;
        core.step(frame, actuators);
        timestamp += STEP_US;
    };
    auto estimatedUpZ = [&]() {
        const mark4::Quaternion &q = core.attitude();
        return q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
    };

    // Fly until the estimate crosses the tilt threshold: the streak begins.
    for (std::uint32_t i = 0U; i < 5000U && estimatedUpZ() >= mark4::FlightCore::CUTOFF_TILT_MIN_UP;
         ++i)
    {
        stepOnce();
    }
    REQUIRE(estimatedUpZ() < mark4::FlightCore::CUTOFF_TILT_MIN_UP);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);

    // 100 ms into the 300 ms confirmation window: kill, and stay killed for
    // well over the confirmation time.
    for (std::uint32_t i = 0U; i < 50U; ++i)
    {
        stepOnce();
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    frame.rc.killSwitch = true;
    for (std::uint32_t i = 0U; i < 250U; ++i)
    {
        stepOnce();
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);

    // Releasing the kill re-enters piloted flight (the estimate is still
    // tilted): a stale streak would cut immediately, a designed reset
    // demands the full confirmation again.
    frame.rc.killSwitch = false;
    stepOnce();
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    stepOnce();
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);

    // The tilt is real and sustained: the fresh confirmation still ends in
    // a cutoff after its full window.
    for (std::uint32_t i = 0U; i < 200U && core.flightPhase() == mark4::FlightPhase::MANUAL; ++i)
    {
        stepOnce();
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
}

TEST_CASE("a recovery that never levels times out into a cutoff")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = playThrow(core, actuators);

    // Tumble slowly through the coast and into the recovery: pure gyro
    // integration (0 g gates the accel correction) rolls the estimate far
    // from level, and the simulated gyro then goes quiet, so the rate loop
    // never sees the drone actually leveling.
    mark4::SensorFrame frame;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.gyroRadS = {3.0f, 0.0f, 0.0f};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;
    for (std::uint32_t i = 0U; i < 250U; ++i)
    {
        frame.timestampUs = timestamp;
        core.step(frame, actuators);
        timestamp += STEP_US;
    }
    REQUIRE(core.flightPhase() == mark4::FlightPhase::RECOVERY);

    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    std::uint32_t elapsed = 0U;
    while (core.flightPhase() == mark4::FlightPhase::RECOVERY && elapsed < 1500U)
    {
        frame.timestampUs = timestamp;
        core.step(frame, actuators);
        timestamp += STEP_US;
        ++elapsed;
    }

    // About RECOVERY_TIMEOUT_US of pushing blind in total (part of the
    // window elapsed while still tumbling), then the cutoff - never an
    // instant trip, never a hover.
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(elapsed >= 700U);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("saturated rates during the ballistic coast cut instead of spinning up")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = playThrow(core, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::BALLISTIC);

    // The gyro pegs near its full scale mid-coast: the attitude is lost,
    // spinning up would fly blind. The core must fall inert.
    mark4::SensorFrame frame;
    frame.timestampUs = timestamp;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.gyroRadS = {35.0f, 0.0f, 0.0f};
    frame.baroPa = HELPER_BARO_PA;
    frame.imuValid = true;
    frame.baroValid = true;
    core.step(frame, actuators);

    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);

    // Still latched on the next quiet frame.
    frame.gyroRadS = {0.0f, 0.0f, 0.0f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.timestampUs = timestamp + STEP_US;
    core.step(frame, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::CUTOFF);
    REQUIRE(actuators.motor[0] == 0.0f);
}

TEST_CASE("the altitude-auto throttle maps to a vertical velocity setpoint around mid stick")
{
    // Three cores flown into a hover and taken over at three stick
    // positions: the commanded collective must order climb > hold > sink,
    // with hold near the hover collective.
    auto collectiveFor = [](float throttle) {
        mark4::FlightCore core;
        mark4::ActuatorFrame actuators;
        std::uint64_t timestamp = playThrow(core, actuators);
        for (std::uint32_t i = 0U; i < 500U && core.flightPhase() != mark4::FlightPhase::HOVER; ++i)
        {
            timestamp = feed(core, actuators, timestamp, 1U, 0.0f, true);
        }
        REQUIRE(core.flightPhase() == mark4::FlightPhase::HOVER);
        // Recentre to arm the takeover, then off centre to take over, then
        // hold the tested position.
        timestamp = feed(core, actuators, timestamp, 1U, mark4::GRAVITY_MPS2, true);
        timestamp = feed(core,
                         actuators,
                         timestamp,
                         1U,
                         mark4::GRAVITY_MPS2,
                         true,
                         0.0f,
                         mark4::PilotMode::ALTITUDE_AUTO,
                         0.7f);
        for (std::uint32_t i = 0U; i < 5U; ++i)
        {
            timestamp = feed(core,
                             actuators,
                             timestamp,
                             1U,
                             mark4::GRAVITY_MPS2,
                             true,
                             0.0f,
                             mark4::PilotMode::ALTITUDE_AUTO,
                             throttle);
        }
        REQUIRE(core.flightPhase() == mark4::FlightPhase::ALTITUDE_AUTO);
        return actuators.motor[0];
    };

    const float sink = collectiveFor(0.15f);
    const float hold = collectiveFor(0.5f);
    const float climb = collectiveFor(1.0f);
    REQUIRE(climb > hold);
    REQUIRE(hold > sink);
    REQUIRE(hold > 0.3f);
    REQUIRE(hold < 0.8f);
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

namespace
{
    /// @brief Feeds frames whose IMU (and optionally baro) is flagged invalid,
    ///        as a platform does when the sensor could not be read: zeros,
    ///        the flag down. RC armed, altitude-auto interlock satisfied.
    /// @return timestamp to continue the stream from
    std::uint64_t feedInvalid(mark4::FlightCore &core,
                              mark4::ActuatorFrame &actuators,
                              std::uint64_t fromUs,
                              std::uint32_t steps,
                              bool armSwitch,
                              bool baroValid = false,
                              mark4::PilotMode mode = mark4::PilotMode::ALTITUDE_AUTO,
                              float throttle = 0.5f)
    {
        return feed(core,
                    actuators,
                    fromUs,
                    steps,
                    0.0f,
                    armSwitch,
                    0.0f,
                    mode,
                    throttle,
                    false,
                    baroValid);
    }

    /// @return true when the two quaternions are bitwise equal
    bool sameAttitude(const mark4::Quaternion &a, const mark4::Quaternion &b)
    {
        return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
    }
} // namespace

TEST_CASE("an invalid IMU with the motors off integrates nothing and refuses to arm")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = settle(core, actuators);
    const mark4::Quaternion before = core.attitude();
    const float altitudeBefore = core.altitudeM();
    const std::uint32_t throwsBefore = core.throwDetector().throwCount();

    // A dead IMU, arm switch on, stick centered: nothing moves, nothing arms.
    timestamp = feedInvalid(core, actuators, timestamp, 100U, true);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(core.imuInvalidRun() == 100U);
    REQUIRE(sameAttitude(core.attitude(), before));
    REQUIRE(core.altitudeM() == altitudeBefore);
    REQUIRE(core.throwDetector().throwCount() == throwsBefore);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // No latch: the sensor coming back is enough, the same arm request
    // now goes through.
    feed(core, actuators, timestamp, 1U, mark4::GRAVITY_MPS2, true);
    REQUIRE(core.imuInvalidRun() == 0U);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
}

TEST_CASE("an invalid IMU under the fault threshold holds the last command")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = enterManual(core, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     20U,
                     mark4::GRAVITY_MPS2,
                     true,
                     0.0f,
                     mark4::PilotMode::MANUAL,
                     0.5f);
    const std::array<float, 4> held = actuators.motor;
    REQUIRE(held[0] > 0.0f);

    feedInvalid(core,
                actuators,
                timestamp,
                mark4::FlightCore::IMU_FAULT_FRAMES - 1U,
                true,
                true,
                mark4::PilotMode::MANUAL,
                0.5f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(actuators.motor == held);
}

TEST_CASE("an invalid IMU with the motors running latches the fault and cuts the motors")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = enterManual(core, actuators);
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     20U,
                     mark4::GRAVITY_MPS2,
                     true,
                     0.0f,
                     mark4::PilotMode::MANUAL,
                     0.5f);
    REQUIRE(actuators.motor[0] > 0.0f);

    timestamp = feedInvalid(core,
                            actuators,
                            timestamp,
                            mark4::FlightCore::IMU_FAULT_FRAMES,
                            true,
                            true,
                            mark4::PilotMode::MANUAL,
                            0.5f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::FAULT);
    REQUIRE(!core.armed());
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Valid frames again, still armed, stick up: latched, motors stay cut.
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     50U,
                     mark4::GRAVITY_MPS2,
                     true,
                     0.0f,
                     mark4::PilotMode::MANUAL,
                     0.5f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::FAULT);
    for (const float m : actuators.motor)
    {
        REQUIRE(m == 0.0f);
    }

    // Releasing the arm switch is not enough either: only the kill leaves.
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     5U,
                     mark4::GRAVITY_MPS2,
                     false,
                     0.0f,
                     mark4::PilotMode::MANUAL,
                     0.0f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::FAULT);
    mark4::SensorFrame kill;
    kill.timestampUs = timestamp;
    kill.rc.killSwitch = true;
    core.step(kill, actuators);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
}

TEST_CASE("an invalid baro on the ground refuses to arm and is not a fault")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = settle(core, actuators);

    timestamp = feed(core,
                     actuators,
                     timestamp,
                     100U,
                     mark4::GRAVITY_MPS2,
                     true,
                     0.0f,
                     mark4::PilotMode::ALTITUDE_AUTO,
                     0.5f,
                     true,
                     false);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(core.imuInvalidRun() == 0U);

    feed(core, actuators, timestamp, 1U, mark4::GRAVITY_MPS2, true);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
}

TEST_CASE("an invalid baro in flight keeps flying on the inertial estimate")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;
    std::uint64_t timestamp = enterManual(core, actuators);
    timestamp = feed(core,
                     actuators,
                     timestamp,
                     20U,
                     mark4::GRAVITY_MPS2,
                     true,
                     0.0f,
                     mark4::PilotMode::MANUAL,
                     0.5f);
    const float baroAltitude = core.baroAltitudeM();

    // Level, 1 g, no baro for a full second: the flight goes on and the
    // raw baro channel stops moving while the fused estimate coasts.
    feed(core,
         actuators,
         timestamp,
         500U,
         mark4::GRAVITY_MPS2,
         true,
         0.0f,
         mark4::PilotMode::MANUAL,
         0.5f,
         true,
         false);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(actuators.motor[0] > 0.0f);
    REQUIRE(core.baroAltitudeM() == baroAltitude);
    REQUIRE(std::fabs(core.altitudeM()) < 0.5f);
}
