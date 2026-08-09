#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/flight_core.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/tuning_table.hpp"
#include "flight_core/types.hpp"
#include "flight_core/vertical_controller.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream

    /// Plausible static pressure carried by every helper frame. The default
    /// SensorFrame reads 0 Pa, a faulted sensor: a core fed with it refuses
    /// to fly, so any test that flies has to provide a real baro.
    constexpr float HELPER_BARO_PA = 101325.0f;

    /// @return live value of a parameter, or NaN when the id is unknown
    float readParam(const mark4::TuningTable &table, std::uint16_t id)
    {
        float value = std::numeric_limits<float>::quiet_NaN();
        static_cast<void>(table.get(id, value));
        return value;
    }

    /// @brief Feeds frames with the given accel on z and a fixed gyro reading.
    /// @return timestamp to continue the stream from
    std::uint64_t feed(mark4::FlightCore &core,
                       mark4::ActuatorFrame &actuators,
                       std::uint64_t fromUs,
                       std::uint32_t steps,
                       float accelZ,
                       float throttle = 0.0f,
                       bool armSwitch = false,
                       float gyroX = 0.0f)
    {
        std::uint64_t timestamp = fromUs;
        for (std::uint32_t i = 0U; i < steps; ++i)
        {
            mark4::SensorFrame frame;
            frame.timestampUs = timestamp;
            frame.rc.killSwitch = false;
            frame.rc.armSwitch = armSwitch;
            frame.rc.throttle = throttle;
            frame.accelMps2 = {0.0f, 0.0f, accelZ};
            frame.gyroRadS = {gyroX, 0.0f, 0.0f};
            frame.baroPa = HELPER_BARO_PA;
            core.step(frame, actuators);
            timestamp += STEP_US;
        }
        return timestamp;
    }
} // namespace

TEST_CASE("the tuning registry is well formed")
{
    const mark4::TuningTable table;

    for (std::size_t index = 0U; index < mark4::TuningTable::PARAM_COUNT; ++index)
    {
        const mark4::TuningParam *param = table.info(index);
        REQUIRE(param != nullptr);

        // A zero id is never handed out: it is the "no parameter" value.
        REQUIRE(param->id != 0U);

        // Ids are unique, so a value can never be routed to two modules.
        for (std::size_t other = 0U; other < index; ++other)
        {
            REQUIRE(table.info(other)->id != param->id);
        }

        // Names are non-empty printable ASCII, zero padded to the field width.
        REQUIRE(param->name[0] != '\0');
        bool padding = false;
        for (const char c : param->name)
        {
            if (c == '\0')
            {
                padding = true;
            }
            else
            {
                REQUIRE(!padding); // no character after the padding starts
                REQUIRE(c >= 0x20);
                REQUIRE(c < 0x7F);
            }
        }

        // The default sits inside the range the table will enforce.
        REQUIRE(param->minValue <= param->maxValue);
        REQUIRE(param->value >= param->minValue);
        REQUIRE(param->value <= param->maxValue);
    }

    REQUIRE(table.info(mark4::TuningTable::PARAM_COUNT) == nullptr);
}

TEST_CASE("the registry defaults are the owning modules' constants")
{
    const mark4::TuningTable table;

    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) ==
            mark4::RateController::DEFAULT_KP_ROLL_PITCH);
    REQUIRE(readParam(table, mark4::TUNING_ID_HOVER_COLLECTIVE) ==
            mark4::VerticalController::DEFAULT_HOVER_COLLECTIVE);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KI) == mark4::AttitudeEstimator::DEFAULT_KI);
}

TEST_CASE("an unknown id is rejected and changes nothing")
{
    mark4::TuningTable table;
    const float before = readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH);

    REQUIRE(table.set(9999U, 1.0f, false) == mark4::TuningStatus::UNKNOWN_ID);

    float value = -1.0f;
    REQUIRE(table.get(9999U, value) == mark4::TuningStatus::UNKNOWN_ID);
    REQUIRE(value == -1.0f); // untouched on failure
    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) == before);
}

TEST_CASE("the tuning bounds are inclusive and reject NaN")
{
    mark4::TuningTable table;
    const mark4::TuningParam *param = table.info(0U);
    REQUIRE(param != nullptr);
    const std::uint16_t id = param->id;
    const float minValue = param->minValue;
    const float maxValue = param->maxValue;
    const float original = param->value;

    // Outside the range, either way: refused, and the live value survives.
    REQUIRE(table.set(id, minValue - 1.0f, false) == mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);
    REQUIRE(table.set(id, maxValue + 1.0f, false) == mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);

    // A NaN passes no comparison, so the acceptance test rejects it.
    REQUIRE(table.set(id, std::numeric_limits<float>::quiet_NaN(), false) ==
            mark4::TuningStatus::OUT_OF_BOUNDS);
    REQUIRE(readParam(table, id) == original);

    // Both edges are valid values.
    REQUIRE(table.set(id, minValue, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, id) == minValue);
    REQUIRE(table.set(id, maxValue, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, id) == maxValue);
}

TEST_CASE("the armed lock only guards the parameters that ask for it")
{
    mark4::TuningTable table;

    // An estimator gain reinterprets the state the stack flies on: locked.
    REQUIRE(table.set(mark4::TUNING_ID_AHRS_KP, 3.0f, true) ==
            mark4::TuningStatus::LOCKED_WHILE_ARMED);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KP) == mark4::AttitudeEstimator::DEFAULT_KP);

    // A controller gain is exactly what a pilot retunes in flight.
    REQUIRE(table.set(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.05f, true) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, mark4::TUNING_ID_RATE_KP_ROLL_PITCH) == 0.05f);

    // Disarmed, the locked one moves.
    REQUIRE(table.set(mark4::TUNING_ID_AHRS_KP, 3.0f, false) == mark4::TuningStatus::OK);
    REQUIRE(readParam(table, mark4::TUNING_ID_AHRS_KP) == 3.0f);
}

TEST_CASE("the flight core derives the armed state from its own phase")
{
    mark4::FlightCore core;
    mark4::ActuatorFrame actuators;

    // The registry is reachable through the core, for a ground station that
    // discovers the parameters instead of hardcoding them.
    REQUIRE(mark4::FlightCore::ParamCount() == mark4::TuningTable::PARAM_COUNT);
    REQUIRE(core.paramInfo(0U) != nullptr);
    REQUIRE(core.paramInfo(mark4::FlightCore::ParamCount()) == nullptr);

    // At rest, stick down: on the ground, everything may be retuned.
    std::uint64_t timestamp = feed(core, actuators, 0U, 200U, mark4::GRAVITY_MPS2);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(core.setParam(mark4::TUNING_ID_AHRS_KP, 3.0f) == mark4::TuningStatus::OK);

    // Stick up: flying. The estimator gain is locked, the rate gain is not.
    timestamp = feed(core, actuators, timestamp, 5U, mark4::GRAVITY_MPS2, 0.5f);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(core.setParam(mark4::TUNING_ID_AHRS_KP, 4.0f) ==
            mark4::TuningStatus::LOCKED_WHILE_ARMED);
    REQUIRE(core.setParam(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.04f) == mark4::TuningStatus::OK);

    // The refused value landed nowhere.
    float value = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_AHRS_KP, value) == mark4::TuningStatus::OK);
    REQUIRE(value == 3.0f);

    // Stick down again: back on the ground, the lock is released.
    feed(core, actuators, timestamp, 5U, mark4::GRAVITY_MPS2);
    REQUIRE(core.flightPhase() == mark4::FlightPhase::IDLE);
    REQUIRE(core.setParam(mark4::TUNING_ID_AHRS_KP, 4.0f) == mark4::TuningStatus::OK);
}

TEST_CASE("a tuned gain reaches the motors on the very next step")
{
    mark4::FlightCore tuned;
    mark4::FlightCore reference;
    mark4::FlightCore control;
    mark4::ActuatorFrame tunedOut;
    mark4::ActuatorFrame referenceOut;
    mark4::ActuatorFrame controlOut;

    // Stick flight with a standing roll rate: the rate loop has a real error
    // to chew on, so its proportional gain shows up in the motor commands.
    const float gyro = 0.3f;
    const std::uint64_t timestamp =
        feed(tuned, tunedOut, 0U, 100U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);
    feed(reference, referenceOut, 0U, 100U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);
    feed(control, controlOut, 0U, 100U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);
    REQUIRE(tuned.flightPhase() == mark4::FlightPhase::MANUAL);

    REQUIRE(tuned.setParam(mark4::TUNING_ID_RATE_KP_ROLL_PITCH,
                           2.0f * mark4::RateController::DEFAULT_KP_ROLL_PITCH) ==
            mark4::TuningStatus::OK);

    feed(tuned, tunedOut, timestamp, 1U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);
    feed(reference, referenceOut, timestamp, 1U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);
    feed(control, controlOut, timestamp, 1U, mark4::GRAVITY_MPS2, 0.5f, false, gyro);

    // The new gain is in effect for the whole of that step, not the one after.
    REQUIRE(tunedOut.motor[0] != referenceOut.motor[0]);

    // And nothing else moved: an untouched core still matches bit for bit.
    for (std::size_t motor = 0U; motor < controlOut.motor.size(); ++motor)
    {
        REQUIRE(controlOut.motor[motor] == referenceOut.motor[motor]);
    }
}

TEST_CASE("the tuned hover collective reaches the recovery feedforward")
{
    // The recovery scales the hover feedforward with the estimated tilt, so
    // the value it scales must be the tuned one, not a frozen default.
    auto recoveryMotor = [](mark4::FlightCore &core) {
        mark4::ActuatorFrame actuators;
        // Rest settles the estimators and the baro reference, then 100 ms of
        // 5 g thrust leaves at about 4 m/s, then free fall to the apex.
        std::uint64_t timestamp = feed(core, actuators, 0U, 200U, mark4::GRAVITY_MPS2, 0.0f, true);
        timestamp = feed(core, actuators, timestamp, 50U, 5.0f * mark4::GRAVITY_MPS2, 0.0f, true);
        float observed = 0.0f;
        for (std::uint32_t i = 0U; i < 1000U && observed == 0.0f; ++i)
        {
            timestamp = feed(core, actuators, timestamp, 1U, 0.0f, 0.0f, true);
            if (core.flightPhase() == mark4::FlightPhase::RECOVERY)
            {
                observed = actuators.motor[0];
            }
        }
        return observed;
    };

    mark4::FlightCore tuned;
    REQUIRE(tuned.setParam(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.7f) == mark4::TuningStatus::OK);
    mark4::FlightCore reference;

    const float tunedMotor = recoveryMotor(tuned);
    const float referenceMotor = recoveryMotor(reference);

    REQUIRE(referenceMotor > 0.0f);
    REQUIRE(tunedMotor > referenceMotor);
}
