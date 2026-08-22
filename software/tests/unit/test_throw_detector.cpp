#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/throw_detector.hpp"
#include "flight_core/types.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream
    constexpr float REST_G = mark4::GRAVITY_MPS2;
    constexpr float THRUST_G = 4.0f * mark4::GRAVITY_MPS2;
    constexpr float RELEASE_VZ = 5.0f;
    constexpr float RELEASE_ALT = 1.2f;

    /// @brief Feeds frames with the given accel norm on z.
    /// @return timestamp to continue the stream from
    std::uint64_t feed(mark4::ThrowDetector &detector,
                       std::uint64_t fromUs,
                       std::uint32_t steps,
                       float accelZ,
                       float vz = 0.0f,
                       float altitude = 0.0f)
    {
        std::uint64_t timestamp = fromUs;
        for (std::uint32_t i = 0U; i < steps; ++i)
        {
            mark4::SensorFrame frame;
            frame.timestampUs = timestamp;
            frame.accelMps2 = {0.0f, 0.0f, accelZ};
            detector.update(frame, vz, altitude);
            timestamp += STEP_US;
        }
        return timestamp;
    }

    /// @brief Plays a complete valid throw signature.
    /// @return timestamp to continue the stream from
    std::uint64_t playThrow(mark4::ThrowDetector &detector)
    {
        std::uint64_t timestamp = feed(detector, 0U, 100U, REST_G);
        timestamp = feed(detector, timestamp, 50U, THRUST_G);                 // 100 ms of thrust
        return feed(detector, timestamp, 40U, 0.0f, RELEASE_VZ, RELEASE_ALT); // 80 ms free fall
    }
} // namespace

TEST_CASE("a complete throw signature is detected and the apex predicted")
{
    mark4::ThrowDetector detector;
    playThrow(detector);

    REQUIRE(detector.state() == mark4::ThrowState::BALLISTIC);
    REQUIRE(detector.throwCount() == 1U);
    REQUIRE(detector.releaseVelocityMps() == RELEASE_VZ);

    // t_apex = t_release + vz0 / g, never detected after the fact.
    const auto expectedDelayUs =
        static_cast<std::uint64_t>(RELEASE_VZ / mark4::GRAVITY_MPS2 * 1e6f);
    const std::uint64_t predictedDelayUs =
        detector.apexTimestampUs() - detector.releaseTimestampUs();
    REQUIRE(predictedDelayUs == expectedDelayUs);

    const float expectedApexM =
        RELEASE_ALT + RELEASE_VZ * RELEASE_VZ / (2.0f * mark4::GRAVITY_MPS2);
    REQUIRE(std::fabs(detector.apexAltitudeM() - expectedApexM) < 0.001f);
}

TEST_CASE("a drop without a thrust phase never detects")
{
    mark4::ThrowDetector detector;
    std::uint64_t timestamp = feed(detector, 0U, 100U, REST_G);
    feed(detector, timestamp, 500U, 0.0f, RELEASE_VZ, RELEASE_ALT); // 1 s of free fall

    REQUIRE(detector.state() == mark4::ThrowState::IDLE);
    REQUIRE(detector.throwCount() == 0U);
}

TEST_CASE("a thrust spike shorter than the minimum is rejected at confirmation")
{
    mark4::ThrowDetector detector;
    std::uint64_t timestamp = feed(detector, 0U, 100U, REST_G);
    timestamp = feed(detector, timestamp, 5U, THRUST_G); // 10 ms only
    feed(detector, timestamp, 100U, 0.0f, RELEASE_VZ, RELEASE_ALT);

    REQUIRE(detector.state() == mark4::ThrowState::IDLE);
    REQUIRE(detector.throwCount() == 0U);
}

TEST_CASE("a hand carry with no release times out back to idle")
{
    mark4::ThrowDetector detector;
    std::uint64_t timestamp = feed(detector, 0U, 100U, REST_G);
    timestamp = feed(detector, timestamp, 50U, THRUST_G);
    feed(detector, timestamp, 200U, REST_G); // 400 ms back at 1 g, no free fall

    REQUIRE(detector.state() == mark4::ThrowState::IDLE);
    REQUIRE(detector.throwCount() == 0U);
}

TEST_CASE("a release too slow for a real throw is rejected")
{
    mark4::ThrowDetector detector;
    std::uint64_t timestamp = feed(detector, 0U, 100U, REST_G);
    timestamp = feed(detector, timestamp, 50U, THRUST_G);
    feed(detector, timestamp, 40U, 0.0f, 1.0f, RELEASE_ALT);

    REQUIRE(detector.state() == mark4::ThrowState::IDLE);
    REQUIRE(detector.throwCount() == 0U);
}

TEST_CASE("landing ends the ballistic phase and keeps the prediction")
{
    mark4::ThrowDetector detector;
    std::uint64_t timestamp = playThrow(detector);
    REQUIRE(detector.state() == mark4::ThrowState::BALLISTIC);

    feed(detector, timestamp, 75U, REST_G); // 150 ms on the ground

    REQUIRE(detector.state() == mark4::ThrowState::IDLE);
    REQUIRE(detector.throwCount() == 1U);
    REQUIRE(detector.releaseVelocityMps() == RELEASE_VZ);
}
