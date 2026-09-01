#include <array>
#include <cmath>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_controller.hpp"
#include "flight_core/mixer.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/types.hpp"
#include "flight_core/vertical_controller.hpp"

TEST_CASE("the mixer spreads the collective evenly with no torque demand")
{
    const std::array<float, 4> motors = mark4::mixMotors(0.5f, {0.0f, 0.0f, 0.0f});
    for (const float m : motors)
    {
        REQUIRE(m == 0.5f);
    }
}

TEST_CASE("a positive roll demand raises the left motors and lowers the right ones")
{
    const std::array<float, 4> motors = mark4::mixMotors(0.5f, {0.1f, 0.0f, 0.0f});
    REQUIRE(motors[2] > 0.5f); // rear left
    REQUIRE(motors[3] > 0.5f); // front left
    REQUIRE(motors[0] < 0.5f); // rear right
    REQUIRE(motors[1] < 0.5f); // front right
}

TEST_CASE("a positive pitch demand raises the rear motors")
{
    const std::array<float, 4> motors = mark4::mixMotors(0.5f, {0.0f, 0.1f, 0.0f});
    REQUIRE(motors[0] > 0.5f); // rear right
    REQUIRE(motors[2] > 0.5f); // rear left
    REQUIRE(motors[1] < 0.5f); // front right
    REQUIRE(motors[3] < 0.5f); // front left
}

TEST_CASE("a positive yaw demand raises one diagonal and lowers the other")
{
    const std::array<float, 4> motors = mark4::mixMotors(0.5f, {0.0f, 0.0f, 0.1f});
    REQUIRE(motors[1] > 0.5f); // (1, 2) diagonal produces positive yaw torque
    REQUIRE(motors[2] > 0.5f);
    REQUIRE(motors[0] < 0.5f);
    REQUIRE(motors[3] < 0.5f);
}

TEST_CASE("the mixer clamps every command to the motor range")
{
    const std::array<float, 4> motors = mark4::mixMotors(0.9f, {0.5f, -0.5f, 0.0f});
    for (const float m : motors)
    {
        REQUIRE(m >= 0.0f);
        REQUIRE(m <= 1.0f);
    }
}

TEST_CASE("the rate controller pushes toward the setpoint and clamps its integrator")
{
    mark4::RateController controller;

    // Setpoint above measurement: positive torque demand on that axis.
    std::array<float, 3> torque = controller.update({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.002f);
    REQUIRE(torque[0] > 0.0f);
    REQUIRE(torque[1] == 0.0f);
    REQUIRE(torque[2] == 0.0f);

    // A persistent error saturates the integrator at its clamp, not beyond.
    for (int i = 0; i < 100000; ++i)
    {
        torque = controller.update({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.002f);
    }
    const float clamped = torque[0];
    torque = controller.update({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.002f);
    REQUIRE(torque[0] == clamped);

    controller.reset();
    torque = controller.update({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.002f);
    REQUIRE(torque[0] == 0.0f);
}

TEST_CASE("the injected default gains behave exactly like the implicit ones")
{
    mark4::RateController implicitGains;
    mark4::RateController explicitGains{mark4::RateController::DEFAULT_KP_ROLL_PITCH,
                                        mark4::RateController::DEFAULT_KI_ROLL_PITCH,
                                        mark4::RateController::DEFAULT_KP_YAW,
                                        mark4::RateController::DEFAULT_KI_YAW};

    // Same sequence through both: the outputs must match bit for bit, so
    // making the gains injectable changed no arithmetic at all.
    for (int i = 0; i < 100; ++i)
    {
        const float phase = 0.1f * static_cast<float>(i);
        const std::array<float, 3> setpoint = {1.0f, -0.5f, 0.25f * phase};
        const std::array<float, 3> gyro = {0.1f * phase, 0.0f, -0.2f};
        const std::array<float, 3> fromImplicit = implicitGains.update(setpoint, gyro, 0.002f);
        const std::array<float, 3> fromExplicit = explicitGains.update(setpoint, gyro, 0.002f);
        for (std::size_t axis = 0U; axis < fromImplicit.size(); ++axis)
        {
            REQUIRE(fromImplicit[axis] == fromExplicit[axis]);
        }
    }
}

TEST_CASE("the attitude controller commands rates that reduce the tilt")
{
    const mark4::AttitudeController controller;

    // Rolled left by 0.2 rad (positive about body x): the corrective rate
    // setpoint must be negative about x, and zero yaw always.
    const float half = 0.1f;
    const mark4::Quaternion rolled{std::cos(half), std::sin(half), 0.0f, 0.0f};
    std::array<float, 3> setpoint = controller.rateSetpointRadS(rolled);
    REQUIRE(setpoint[0] < 0.0f);
    REQUIRE(std::fabs(setpoint[1]) < 1e-6f);
    REQUIRE(setpoint[2] == 0.0f);

    // Pitched up by 0.2 rad (positive about body y): corrective about y.
    const mark4::Quaternion pitched{std::cos(half), 0.0f, std::sin(half), 0.0f};
    setpoint = controller.rateSetpointRadS(pitched);
    REQUIRE(setpoint[1] < 0.0f);
    REQUIRE(std::fabs(setpoint[0]) < 1e-6f);

    // Level: nothing to do.
    setpoint = controller.rateSetpointRadS(mark4::Quaternion{});
    REQUIRE(std::fabs(setpoint[0]) < 1e-6f);
    REQUIRE(std::fabs(setpoint[1]) < 1e-6f);
}

TEST_CASE("the vertical controller climbs on a positive setpoint around the feedforward")
{
    mark4::VerticalController controller;

    const float hover = controller.update(0.0f, 0.0f, 0.002f);
    REQUIRE(std::fabs(hover - mark4::VerticalController::DEFAULT_HOVER_COLLECTIVE) < 0.01f);

    controller.reset();
    const float climbing = controller.update(1.0f, 0.0f, 0.002f);
    REQUIRE(climbing > hover);

    controller.reset();
    const float sinking = controller.update(-1.0f, 0.0f, 0.002f);
    REQUIRE(sinking < hover);
}

TEST_CASE("a tilted desired up leans the drone against a forward drift")
{
    const mark4::AttitudeController controller;

    // Drifting toward world +x, the braking target tilts the thrust toward
    // -x: at a level attitude the commanded pitch rate must raise the nose
    // (negative about body y, which points left), and nothing else.
    const std::array<float, 3> brakeUp = {-0.2f, 0.0f, 0.9798f};
    const std::array<float, 3> setpoint = controller.rateSetpointRadS(mark4::Quaternion{}, brakeUp);
    REQUIRE(setpoint[1] < 0.0f);
    REQUIRE(std::fabs(setpoint[0]) < 1e-6f);
    REQUIRE(setpoint[2] == 0.0f);

    // Reaching the desired lean means no more correction.
    const float half = 0.5f * std::asin(0.2f);
    const mark4::Quaternion leaned{std::cos(half), 0.0f, -std::sin(half), 0.0f};
    const std::array<float, 3> settled = controller.rateSetpointRadS(leaned, brakeUp);
    REQUIRE(std::fabs(settled[0]) < 1e-3f);
    REQUIRE(std::fabs(settled[1]) < 1e-3f);
}
