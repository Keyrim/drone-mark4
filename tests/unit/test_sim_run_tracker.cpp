/// @file
/// @brief Run integrity: what the trajectory hash is invariant to, what it
///        must never be invariant to, and when it stops moving.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "platform_sim/sim_run_tracker.hpp"

namespace
{
    /// Frames per simulated second of the sequences below.
    constexpr std::uint64_t TICK_US = 2000U;

    /// One step of a synthetic run: the frame and the outputs it drew.
    struct Step
    {
        mark4::SensorFrame frame;      ///< sensor frame of the step
        mark4::ActuatorFrame actuator; ///< actuator outputs of the step
    };

    /// @brief Builds a synthetic run, every field moving frame by frame so a
    ///        hash that ignored one of them would still be caught.
    /// @param startUs simulated time of the first frame [us]
    /// @param frames number of frames
    /// @return the sequence
    std::vector<Step> makeRun(std::uint64_t startUs, std::uint32_t frames)
    {
        std::vector<Step> run;
        run.reserve(frames);
        for (std::uint32_t index = 0U; index < frames; ++index)
        {
            const auto ramp = static_cast<float>(index);
            Step step;
            step.frame.timestampUs = startUs + index * TICK_US;
            step.frame.gyroRadS = {0.5f + ramp, -1.5f - ramp, 2.5f};
            step.frame.accelMps2 = {0.25f, -0.75f - ramp, 9.5f};
            step.frame.baroPa = 101325.0f - ramp;
            step.actuator.timestampUs = step.frame.timestampUs;
            step.actuator.motor = {0.1f + ramp, 0.2f, 0.3f, 0.4f - ramp};
            run.push_back(step);
        }
        return run;
    }

    /// @brief Plays a whole sequence through a fresh tracker.
    /// @param run sequence to play
    /// @param windowUs hash window handed to beginRun, 0 for the default
    /// @return the tracker, run played out
    mark4::SimRunTracker play(const std::vector<Step> &run, std::uint32_t windowUs = 0U)
    {
        mark4::SimRunTracker tracker;
        tracker.beginRun(1U, run.front().frame.timestampUs, windowUs);
        for (const Step &step : run)
        {
            tracker.update(step.frame, step.actuator);
        }
        return tracker;
    }
} // namespace

TEST_CASE("two identical run sequences hash equal")
{
    const auto run = makeRun(0U, 100U);
    REQUIRE(play(run).hash() == play(run).hash());
}

TEST_CASE("the same run shifted in absolute time hashes equal")
{
    // The instant a run starts at is a property of the process uptime, not of
    // the run: relative time is what the hash is defined over.
    const auto early = makeRun(0U, 100U);
    const auto late = makeRun(987'654'321'000U, 100U);
    REQUIRE(play(early).hash() == play(late).hash());
    REQUIRE(play(early).hashedFrames() == play(late).hashedFrames());
}

TEST_CASE("a one bit change anywhere in the run changes the hash")
{
    const auto reference = makeRun(0U, 100U);
    const std::uint64_t expected = play(reference).hash();

    SECTION("in a gyro sample")
    {
        auto altered = reference;
        altered[50].frame.gyroRadS[1] = std::nextafter(altered[50].frame.gyroRadS[1], 1000.0f);
        REQUIRE(play(altered).hash() != expected);
    }
    SECTION("in an accelerometer sample")
    {
        auto altered = reference;
        altered[3].frame.accelMps2[2] = std::nextafter(altered[3].frame.accelMps2[2], 1000.0f);
        REQUIRE(play(altered).hash() != expected);
    }
    SECTION("in the barometer")
    {
        auto altered = reference;
        altered[99].frame.baroPa = std::nextafter(altered[99].frame.baroPa, 0.0f);
        REQUIRE(play(altered).hash() != expected);
    }
    SECTION("in a motor command")
    {
        auto altered = reference;
        altered[0].actuator.motor[3] = std::nextafter(altered[0].actuator.motor[3], 1.0f);
        REQUIRE(play(altered).hash() != expected);
    }
    SECTION("in the pacing of the frames")
    {
        auto altered = reference;
        altered[42].frame.timestampUs += 1U;
        REQUIRE(play(altered).hash() != expected);
    }
}

TEST_CASE("the rc state is not part of the hash")
{
    // RC arrives out-of-band, paced by the host: which frame carries a given
    // arm state is not a property of the run.
    auto armed = makeRun(0U, 20U);
    for (Step &step : armed)
    {
        step.frame.rc.killSwitch = false;
        step.frame.rc.armSwitch = true;
        step.frame.rc.throttle = 0.5f;
    }
    REQUIRE(play(armed).hash() == play(makeRun(0U, 20U)).hash());
}

TEST_CASE("the hash seals at the end of the window and stops moving")
{
    const auto run = makeRun(0U, 100U);
    // Ten frames of window: the eleventh frame seals, the rest change nothing.
    const auto window = static_cast<std::uint32_t>(10U * TICK_US);

    mark4::SimRunTracker tracker;
    tracker.beginRun(1U, 0U, window);
    for (std::size_t index = 0U; index < 10U; ++index)
    {
        tracker.update(run[index].frame, run[index].actuator);
    }
    REQUIRE(!tracker.sealed());
    const std::uint64_t sealedHash = tracker.hash();

    for (std::size_t index = 10U; index < run.size(); ++index)
    {
        tracker.update(run[index].frame, run[index].actuator);
    }
    REQUIRE(tracker.sealed());
    REQUIRE(tracker.hash() == sealedHash);
    REQUIRE(tracker.hashedFrames() == 10U);

    // The window is what the hash covers: a longer one is a different number.
    REQUIRE(play(run, window * 2U).hash() != sealedHash);
}

TEST_CASE("a run that lost a tick latches degraded")
{
    mark4::SimRunTracker tracker;
    tracker.beginRun(1U, 0U, 0U);

    // The first report is the baseline of the run, whatever it carries: the
    // plant counter is cumulative and older runs are none of this run's
    // business.
    tracker.noteLink(17U, 4U);
    REQUIRE(!tracker.degraded());

    tracker.noteLink(17U, 5U);
    REQUIRE(!tracker.degraded()); // resends are answered, not lost

    tracker.noteLink(18U, 5U);
    REQUIRE(tracker.degraded());

    tracker.noteLink(18U, 5U);
    REQUIRE(tracker.degraded()); // never clears inside the run
    REQUIRE(tracker.lockstepTimeouts() == 18U);
    REQUIRE(tracker.duplicateFrames() == 5U);

    // A new run starts clean, on its own baseline.
    tracker.beginRun(2U, 0U, 0U);
    tracker.noteLink(18U, 5U);
    REQUIRE(!tracker.degraded());
    REQUIRE(tracker.runId() == 2U);
}
