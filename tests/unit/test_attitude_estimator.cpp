#include <array>
#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream

    /// @return a frame carrying the given rates and specific force
    mark4::SensorFrame makeFrame(std::uint64_t timestampUs,
                                 const std::array<float, 3> &gyroRadS,
                                 const std::array<float, 3> &accelMps2)
    {
        mark4::SensorFrame frame;
        frame.timestampUs = timestampUs;
        frame.gyroRadS = gyroRadS;
        frame.accelMps2 = accelMps2;
        return frame;
    }

    /// @brief Feeds a constant frame for the given number of steps.
    /// @return timestamp to continue the stream from
    std::uint64_t feed(mark4::AttitudeEstimator &estimator,
                       std::uint64_t fromUs,
                       std::uint32_t steps,
                       const std::array<float, 3> &gyroRadS,
                       const std::array<float, 3> &accelMps2)
    {
        std::uint64_t timestamp = fromUs;
        for (std::uint32_t i = 0U; i < steps; ++i)
        {
            estimator.update(makeFrame(timestamp, gyroRadS, accelMps2));
            timestamp += STEP_US;
        }
        return timestamp;
    }

    /// @return world up axis expressed in the body frame of the estimate
    std::array<float, 3> upInBody(const mark4::Quaternion &q)
    {
        return {2.0f * (q.x * q.z - q.w * q.y),
                2.0f * (q.w * q.x + q.y * q.z),
                q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z};
    }

    constexpr std::array<float, 3> ZERO_RATES = {0.0f, 0.0f, 0.0f};
    constexpr std::array<float, 3> LEVEL_ACCEL = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
} // namespace

TEST_CASE("static level frames keep the attitude at identity")
{
    mark4::AttitudeEstimator estimator;
    feed(estimator, 0U, 1000U, ZERO_RATES, LEVEL_ACCEL);

    const mark4::Quaternion &q = estimator.attitude();
    REQUIRE(q.w > 0.9999f);
    REQUIRE(std::fabs(q.x) < 0.001f);
    REQUIRE(std::fabs(q.y) < 0.001f);
    REQUIRE(std::fabs(q.z) < 0.001f);
}

TEST_CASE("a rotation about the vertical axis integrates into the expected yaw")
{
    mark4::AttitudeEstimator estimator;
    // 1 rad/s about z for exactly 1 s; the accelerometer keeps seeing level
    // gravity, which says nothing about yaw and must not fight the rotation.
    feed(estimator, 0U, 501U, {0.0f, 0.0f, 1.0f}, LEVEL_ACCEL);

    const mark4::Quaternion &q = estimator.attitude();
    REQUIRE(std::fabs(q.w - std::cos(0.5f)) < 0.01f);
    REQUIRE(std::fabs(q.z - std::sin(0.5f)) < 0.01f);
    REQUIRE(std::fabs(q.x) < 0.01f);
    REQUIRE(std::fabs(q.y) < 0.01f);
}

TEST_CASE("the estimate converges to the gravity direction seen by the accelerometer")
{
    mark4::AttitudeEstimator estimator;
    // Body rolled by 0.5 rad: gravity shows up tilted in the body frame.
    const std::array<float, 3> tiltedAccel = {
        0.0f, mark4::GRAVITY_MPS2 * std::sin(0.5f), mark4::GRAVITY_MPS2 * std::cos(0.5f)};
    feed(estimator, 0U, 5000U, ZERO_RATES, tiltedAccel);

    const std::array<float, 3> up = upInBody(estimator.attitude());
    REQUIRE(std::fabs(up[0] - 0.0f) < 0.01f);
    REQUIRE(std::fabs(up[1] - std::sin(0.5f)) < 0.01f);
    REQUIRE(std::fabs(up[2] - std::cos(0.5f)) < 0.01f);
}

TEST_CASE("free fall freezes the gravity correction")
{
    // ki = 0 isolates the gate: with an integral term the (correct) residual
    // bias estimate would keep acting during the ballistic phase.
    mark4::AttitudeEstimator estimator(mark4::AttitudeEstimator::DEFAULT_KP, 0.0f);
    const std::array<float, 3> tiltedAccel = {
        0.0f, mark4::GRAVITY_MPS2 * std::sin(0.5f), mark4::GRAVITY_MPS2 * std::cos(0.5f)};
    const std::uint64_t timestamp = feed(estimator, 0U, 5000U, ZERO_RATES, tiltedAccel);
    const mark4::Quaternion before = estimator.attitude();

    // 0 g and no rotation: pure gyro integration of zero rates, the attitude
    // must not drift back toward anything.
    feed(estimator, timestamp, 1000U, ZERO_RATES, {0.0f, 0.0f, 0.0f});

    const mark4::Quaternion &after = estimator.attitude();
    REQUIRE(std::fabs(after.w - before.w) < 1e-5f);
    REQUIRE(std::fabs(after.x - before.x) < 1e-5f);
    REQUIRE(std::fabs(after.y - before.y) < 1e-5f);
    REQUIRE(std::fabs(after.z - before.z) < 1e-5f);
}

TEST_CASE("a constant gyro bias is estimated away on the pad")
{
    mark4::AttitudeEstimator estimator;
    // The gyro reports a rotation that the accelerometer contradicts: the
    // integral term must absorb it as a bias estimate.
    feed(estimator, 0U, 30000U, {0.05f, 0.0f, 0.0f}, LEVEL_ACCEL);

    REQUIRE(std::fabs(estimator.gyroBiasRadS()[0] - 0.05f) < 0.005f);
    REQUIRE(estimator.attitude().w > 0.999f);
}

TEST_CASE("a gap in the stream is skipped instead of integrated")
{
    mark4::AttitudeEstimator estimator;
    const std::uint64_t timestamp = feed(estimator, 0U, 1000U, ZERO_RATES, LEVEL_ACCEL);
    const mark4::Quaternion before = estimator.attitude();

    // One frame a full second later with a violent rate: integrating it over
    // the gap would slew the attitude by half a turn.
    const std::uint64_t afterGapUs = timestamp + 1000000U;
    estimator.update(makeFrame(afterGapUs, {3.14f, 0.0f, 0.0f}, LEVEL_ACCEL));

    const mark4::Quaternion &after = estimator.attitude();
    REQUIRE(std::fabs(after.w - before.w) < 1e-6f);
    REQUIRE(std::fabs(after.x - before.x) < 1e-6f);
}
