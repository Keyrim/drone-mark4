#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "flight_core/vertical_estimator.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream
    constexpr float STEP_S = 0.002f;

    /// Standard atmosphere pressure at an absolute altitude, mirroring the
    /// simulator's sensor model (the inverse of pressureAltitudeM).
    float pressureAtAltitude(float altitudeM)
    {
        return 101325.0f * std::pow(1.0f - 2.25577e-5f * altitudeM, 5.25588f);
    }

    /// @return a frame at the given absolute altitude, with the given specific force
    mark4::SensorFrame makeFrame(std::uint64_t timestampUs, float altitudeM, float accelZ)
    {
        mark4::SensorFrame frame;
        frame.timestampUs = timestampUs;
        frame.accelMps2 = {0.0f, 0.0f, accelZ};
        frame.baroPa = pressureAtAltitude(altitudeM);
        return frame;
    }

    /// @brief Runs the reference capture phase at a constant altitude.
    /// @return timestamp to continue the stream from
    std::uint64_t captureReference(mark4::VerticalEstimator &estimator, float altitudeM)
    {
        std::uint64_t timestamp = 0U;
        for (std::uint32_t i = 0U; i < mark4::VerticalEstimator::REFERENCE_SAMPLES; ++i)
        {
            estimator.update(makeFrame(timestamp, altitudeM, mark4::GRAVITY_MPS2), STEP_S, {});
            timestamp += STEP_US;
        }
        return timestamp;
    }
} // namespace

TEST_CASE("the estimate is not ready before the baro reference is captured")
{
    mark4::VerticalEstimator estimator;
    mark4::SensorFrame frame = makeFrame(0U, 120.0f, mark4::GRAVITY_MPS2);

    REQUIRE(!estimator.ready());
    estimator.update(frame, STEP_S, {});
    REQUIRE(!estimator.ready());

    captureReference(estimator, 120.0f);
    REQUIRE(estimator.ready());
}

TEST_CASE("at rest the altitude reads zero wherever the estimator woke up")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 350.0f);

    for (std::uint32_t i = 0U; i < 2500U; ++i)
    {
        estimator.update(makeFrame(timestamp, 350.0f, mark4::GRAVITY_MPS2), STEP_S, {});
        timestamp += STEP_US;
    }

    REQUIRE(std::fabs(estimator.altitudeM()) < 0.05f);
    REQUIRE(std::fabs(estimator.verticalVelocityMps()) < 0.05f);
}

TEST_CASE("a steady baro climb converges to the climb rate")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 100.0f);

    // 1 m/s climb reported by the baro alone; the accelerometer keeps seeing
    // 1 g (a constant velocity produces no specific force change).
    constexpr float CLIMB_RATE_MPS = 1.0f;
    float altitude = 100.0f;
    for (std::uint32_t i = 0U; i < 5000U; ++i)
    {
        altitude += CLIMB_RATE_MPS * STEP_S;
        estimator.update(makeFrame(timestamp, altitude, mark4::GRAVITY_MPS2), STEP_S, {});
        timestamp += STEP_US;
    }

    REQUIRE(std::fabs(estimator.verticalVelocityMps() - CLIMB_RATE_MPS) < 0.1f);
    REQUIRE(std::fabs(estimator.altitudeM() - (altitude - 100.0f)) < 0.5f);
}

TEST_CASE("free fall drives the velocity to minus g times t through the accelerometer")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 200.0f);

    // 0 g specific force, ballistic altitude profile from 200 m for 1 s.
    constexpr float FALL_S = 1.0f;
    float time = 0.0f;
    while (time < FALL_S)
    {
        time += STEP_S;
        const float altitude = 200.0f - 0.5f * mark4::GRAVITY_MPS2 * time * time;
        estimator.update(makeFrame(timestamp, altitude, 0.0f), STEP_S, {});
        timestamp += STEP_US;
    }

    REQUIRE(std::fabs(estimator.verticalVelocityMps() + mark4::GRAVITY_MPS2 * FALL_S) < 0.3f);
}

TEST_CASE("an implausible baro sample never seeds the reference")
{
    mark4::VerticalEstimator estimator;

    // A zeroed sensor (the SensorFrame default) for well over the capture
    // window: the estimator must keep refusing to run on it.
    std::uint64_t timestamp = 0U;
    for (std::uint32_t i = 0U; i < 4U * mark4::VerticalEstimator::REFERENCE_SAMPLES; ++i)
    {
        mark4::SensorFrame frame = makeFrame(timestamp, 100.0f, mark4::GRAVITY_MPS2);
        frame.baroPa = 0.0f;
        estimator.update(frame, STEP_S, {});
        timestamp += STEP_US;
    }
    REQUIRE(!estimator.ready());

    // Healthy samples resume the capture where it never started.
    captureReference(estimator, 100.0f);
    REQUIRE(estimator.ready());
}

TEST_CASE("a zeroed baro frame cannot poison the vertical state")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 100.0f);
    for (std::uint32_t i = 0U; i < 500U; ++i)
    {
        estimator.update(makeFrame(timestamp, 100.0f, mark4::GRAVITY_MPS2), STEP_S, {});
        timestamp += STEP_US;
    }
    const float altitudeBefore = estimator.altitudeM();
    const float velocityBefore = estimator.verticalVelocityMps();

    // One glitch frame at 0 Pa (a 26 km baro altitude) used to move the
    // altitude by ~145 m and the velocity by ~207 m/s in a single step.
    mark4::SensorFrame glitch = makeFrame(timestamp, 100.0f, mark4::GRAVITY_MPS2);
    glitch.baroPa = 0.0f;
    estimator.update(glitch, STEP_S, {});

    REQUIRE(std::fabs(estimator.altitudeM() - altitudeBefore) < 0.01f);
    REQUIRE(std::fabs(estimator.verticalVelocityMps() - velocityBefore) < 0.01f);
}

TEST_CASE("a plausible baro jump is bounded by the innovation clamp")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 100.0f);

    // A 500 m step is inside the plausible pressure window but far beyond
    // any real motion between two frames: the pull must be clamped.
    estimator.update(makeFrame(timestamp, 600.0f, mark4::GRAVITY_MPS2), STEP_S, {});

    const float maxAltitudeStep = mark4::VerticalEstimator::DEFAULT_ALTITUDE_GAIN *
                                  mark4::VerticalEstimator::MAX_INNOVATION_M * STEP_S;
    const float maxVelocityStep = mark4::VerticalEstimator::DEFAULT_VELOCITY_GAIN *
                                  mark4::VerticalEstimator::MAX_INNOVATION_M * STEP_S;
    REQUIRE(std::fabs(estimator.altitudeM()) <= maxAltitudeStep + 1e-4f);
    REQUIRE(std::fabs(estimator.verticalVelocityMps()) <= maxVelocityStep + 1e-4f);
}

TEST_CASE("the baro altitude conversion inverts the standard atmosphere")
{
    for (const float altitude : {0.0f, 50.0f, 500.0f, 2000.0f})
    {
        const float roundtrip =
            mark4::VerticalEstimator::pressureAltitudeM(pressureAtAltitude(altitude));
        REQUIRE(std::fabs(roundtrip - altitude) < 0.01f);
    }
}

TEST_CASE("a sideways push is dead reckoned and leaks back toward zero")
{
    mark4::VerticalEstimator estimator;
    std::uint64_t timestamp = captureReference(estimator, 100.0f);
    REQUIRE(estimator.horizontalVelocityMps()[0] == 0.0f);

    // 2 m/s^2 sideways for 1 s, level: about 2 m/s minus the leak.
    constexpr float PUSH_MPS2 = 2.0f;
    for (std::uint32_t i = 0U; i < 500U; ++i)
    {
        mark4::SensorFrame frame = makeFrame(timestamp, 100.0f, mark4::GRAVITY_MPS2);
        frame.accelMps2[0] = PUSH_MPS2;
        estimator.update(frame, STEP_S, {});
        timestamp += STEP_US;
    }
    const float pushed = estimator.horizontalVelocityMps()[0];
    REQUIRE(pushed > 1.7f);
    REQUIRE(pushed < 2.0f);
    REQUIRE(std::fabs(estimator.horizontalVelocityMps()[1]) < 1e-3f);

    // Back at rest, nothing measures the velocity: the leak fades it out.
    for (std::uint32_t i = 0U; i < 1000U; ++i)
    {
        estimator.update(makeFrame(timestamp, 100.0f, mark4::GRAVITY_MPS2), STEP_S, {});
        timestamp += STEP_US;
    }
    REQUIRE(estimator.horizontalVelocityMps()[0] < 0.85f * pushed);
    REQUIRE(estimator.horizontalVelocityMps()[0] > 0.0f);
}
