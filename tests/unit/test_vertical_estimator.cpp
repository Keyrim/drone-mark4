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
            estimator.update(makeFrame(timestamp, altitudeM, mark4::GRAVITY_MPS2), {});
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
    estimator.update(frame, {});
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
        estimator.update(makeFrame(timestamp, 350.0f, mark4::GRAVITY_MPS2), {});
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
        estimator.update(makeFrame(timestamp, altitude, mark4::GRAVITY_MPS2), {});
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
        estimator.update(makeFrame(timestamp, altitude, 0.0f), {});
        timestamp += STEP_US;
    }

    REQUIRE(std::fabs(estimator.verticalVelocityMps() + mark4::GRAVITY_MPS2 * FALL_S) < 0.3f);
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
