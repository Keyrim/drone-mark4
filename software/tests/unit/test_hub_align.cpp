/// @file
/// @brief The one alignment rule of the system, from both ends: the join
///        itself, the scoring frozen against the reference implementation
///        this replaces, and the promise that a number read live and the
///        same number read afterwards on the recording are one number.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "hub/stream_align.hpp"

namespace
{
    /// Relative agreement asked of the scoring against its reference. Both
    /// sides run the same operations in the same order on the same doubles,
    /// so the only room for a difference is the last bit or two of a libm
    /// acos and sqrt; anything above this would be a difference in the
    /// arithmetic itself, which is exactly what must not drift.
    constexpr double REFERENCE_EPSILON = 1e-12;

    /// @brief One sample, spelled by the values the comparison reads.
    /// @param timestampUs instant [us]
    /// @param altitudeM altitude [m]
    /// @param verticalVelocityMps vertical velocity [m/s]
    /// @return the sample, attitude left level
    mark4::AlignSample sampleAt(double timestampUs, double altitudeM, double verticalVelocityMps)
    {
        mark4::AlignSample sample;
        sample.timestampUs = timestampUs;
        sample.attitudeQuat = {1.0, 0.0, 0.0, 0.0};
        sample.altitudeM = altitudeM;
        sample.verticalVelocityMps = verticalVelocityMps;
        return sample;
    }

    /// @brief Path of one committed fixture.
    /// @param name file name
    /// @return the full path
    std::string fixture(const char *name)
    {
        return std::string(MARK4_TEST_FIXTURES) + "/" + name;
    }
} // namespace

TEST_CASE("a telemetry sample takes the exact sample nearest to it")
{
    const std::vector<mark4::AlignSample> telemetry{sampleAt(1000.0, 5.0, 1.0)};
    const std::vector<mark4::AlignSample> simRaw{
        sampleAt(0.0, 1.0, 0.0), sampleAt(900.0, 2.0, 0.0), sampleAt(2000.0, 3.0, 0.0)};

    const std::vector<mark4::AlignedPair> pairs = mark4::alignStreams(telemetry, simRaw);
    REQUIRE(pairs.size() == 1U);
    CHECK(pairs[0].timestampUs == 1000.0);
    CHECK(pairs[0].gapUs == -100.0);
    CHECK(pairs[0].altitudeErrorM == Catch::Approx(3.0));
    CHECK(pairs[0].verticalVelocityErrorMps == Catch::Approx(1.0));
}

TEST_CASE("a tie between two exact samples takes the later one")
{
    // Equally far on either side: the walk advances while the next sample is
    // no worse than the current one, so it settles on the later of the two.
    const std::vector<mark4::AlignSample> telemetry{sampleAt(1000.0, 0.0, 0.0)};
    const std::vector<mark4::AlignSample> simRaw{sampleAt(900.0, 1.0, 0.0),
                                                 sampleAt(1100.0, 2.0, 0.0)};

    const std::vector<mark4::AlignedPair> pairs = mark4::alignStreams(telemetry, simRaw);
    REQUIRE(pairs.size() == 1U);
    CHECK(pairs[0].gapUs == 100.0);
    CHECK(pairs[0].altitudeErrorM == Catch::Approx(-2.0));
}

TEST_CASE("a telemetry sample with nothing near enough is dropped")
{
    const std::vector<mark4::AlignSample> telemetry{
        sampleAt(0.0, 0.0, 0.0), sampleAt(500000.0, 0.0, 0.0), sampleAt(1000000.0, 0.0, 0.0)};
    const std::vector<mark4::AlignSample> simRaw{sampleAt(0.0, 0.0, 0.0),
                                                 sampleAt(1000000.0, 0.0, 0.0)};

    const std::vector<mark4::AlignedPair> pairs = mark4::alignStreams(telemetry, simRaw);
    REQUIRE(pairs.size() == 2U);
    CHECK(pairs[0].timestampUs == 0.0);
    CHECK(pairs[1].timestampUs == 1000000.0);
}

TEST_CASE("a sample exactly at the alignment gap still matches")
{
    const std::vector<mark4::AlignSample> telemetry{sampleAt(0.0, 0.0, 0.0)};
    const std::vector<mark4::AlignSample> justInside{sampleAt(mark4::MAX_ALIGN_GAP_US, 0.0, 0.0)};
    const std::vector<mark4::AlignSample> justOutside{
        sampleAt(mark4::MAX_ALIGN_GAP_US + 1.0, 0.0, 0.0)};

    CHECK(mark4::alignStreams(telemetry, justInside).size() == 1U);
    CHECK(mark4::alignStreams(telemetry, justOutside).empty());
}

TEST_CASE("an empty stream aligns to nothing")
{
    const std::vector<mark4::AlignSample> some{sampleAt(0.0, 0.0, 0.0)};
    CHECK(mark4::alignStreams({}, some).empty());
    CHECK(mark4::alignStreams(some, {}).empty());
    CHECK(mark4::alignStreams({}, {}).empty());

    const mark4::CompareScore score = mark4::scorePairs({}, 0U);
    CHECK(score.alignedSamples == 0U);
    CHECK(score.durationS == 0.0);
    CHECK(score.metrics[0].name == "attitude");
    CHECK(score.metrics[0].worstWindows.empty());
}

TEST_CASE("the rotation between two attitudes is the angle between them")
{
    const std::array<double, 4> level{1.0, 0.0, 0.0, 0.0};
    const double half = std::sqrt(0.5);
    const std::array<double, 4> quarterTurn{half, half, 0.0, 0.0};

    CHECK(mark4::errorAngleDeg(level, level) == Catch::Approx(0.0));
    CHECK(mark4::errorAngleDeg(level, quarterTurn) == Catch::Approx(90.0));
    // A quaternion and its negation are the same rotation.
    const std::array<double, 4> negated{-1.0, 0.0, 0.0, 0.0};
    CHECK(mark4::errorAngleDeg(level, negated) == Catch::Approx(0.0));
}

TEST_CASE("the scoring reproduces the reference numbers of a recorded session")
{
    // The values below were produced by the python scoring this replaces,
    // run on the two fixtures next to this test. They are the contract: the
    // hub now answers what that script answered, to the last digit it had.
    std::vector<mark4::AlignSample> telemetry;
    std::vector<mark4::AlignSample> simRaw;
    REQUIRE(mark4::loadStreamSamples(
        fixture("streams_20260805_213627_telemetry.csv"), mark4::TELEMETRY_COLUMNS, telemetry));
    REQUIRE(mark4::loadStreamSamples(
        fixture("streams_20260805_213627_simraw.csv"), mark4::SIM_RAW_COLUMNS, simRaw));
    REQUIRE(telemetry.size() == 253U);
    REQUIRE(simRaw.size() == 384U);

    const std::vector<mark4::AlignedPair> pairs = mark4::alignStreams(telemetry, simRaw);
    const mark4::CompareScore score = mark4::scorePairs(pairs, telemetry.size() - pairs.size());

    CHECK(score.alignedSamples == 253U);
    CHECK(score.unmatched == 0U);
    CHECK(score.durationS == Catch::Approx(5.039999999999999).epsilon(REFERENCE_EPSILON));

    CHECK(score.metrics[0].name == "attitude");
    CHECK(score.metrics[0].unit == "deg");
    CHECK(score.metrics[0].rms == Catch::Approx(0.4346061183861049).epsilon(REFERENCE_EPSILON));
    CHECK(score.metrics[0].max == Catch::Approx(0.8559195568255976).epsilon(REFERENCE_EPSILON));

    CHECK(score.metrics[1].name == "altitude");
    CHECK(score.metrics[1].unit == "m");
    CHECK(score.metrics[1].rms == Catch::Approx(0.535788074639396).epsilon(REFERENCE_EPSILON));
    CHECK(score.metrics[1].max == Catch::Approx(1.2892004046589136).epsilon(REFERENCE_EPSILON));

    CHECK(score.metrics[2].name == "verticalVelocity");
    CHECK(score.metrics[2].unit == "m/s");
    CHECK(score.metrics[2].rms == Catch::Approx(1.7044332331053493).epsilon(REFERENCE_EPSILON));
    CHECK(score.metrics[2].max == Catch::Approx(5.286142210261914).epsilon(REFERENCE_EPSILON));

    const std::array<std::array<mark4::ScoreWindow, 3>, 3> expected{
        {{{{3.634, 0.7086225670323298}, {4.634, 0.5843281401397385}, {7.634, 0.2882987588311666}}},
         {{{5.634, 0.9377582996915836}, {4.634, 0.7267604071381336}, {6.634, 0.19404603268464157}}},
         {{{4.634, 3.3958661837324935}, {5.634, 1.768273582931189}, {7.634, 0.1935169423002827}}}}};
    for (std::size_t metric = 0U; metric < expected.size(); ++metric)
    {
        INFO("metric " << score.metrics[metric].name);
        REQUIRE(score.metrics[metric].worstWindows.size() == mark4::WORST_WINDOW_COUNT);
        for (std::size_t rank = 0U; rank < mark4::WORST_WINDOW_COUNT; ++rank)
        {
            INFO("rank " << rank);
            CHECK(score.metrics[metric].worstWindows[rank].startS ==
                  Catch::Approx(expected[metric][rank].startS).epsilon(REFERENCE_EPSILON));
            CHECK(score.metrics[metric].worstWindows[rank].rms ==
                  Catch::Approx(expected[metric][rank].rms).epsilon(REFERENCE_EPSILON));
        }
    }
}

TEST_CASE("the live path and the offline path produce the same pairs")
{
    // The regression that matters: a number a page shows while a session
    // runs and the number the recording of that same session gives
    // afterwards must be the same number, or neither is worth reading.
    std::vector<mark4::AlignSample> telemetry;
    std::vector<mark4::AlignSample> simRaw;
    REQUIRE(mark4::loadStreamSamples(
        fixture("streams_20260805_213627_telemetry.csv"), mark4::TELEMETRY_COLUMNS, telemetry));
    REQUIRE(mark4::loadStreamSamples(
        fixture("streams_20260805_213627_simraw.csv"), mark4::SIM_RAW_COLUMNS, simRaw));

    const std::vector<mark4::AlignedPair> offline = mark4::alignStreams(telemetry, simRaw);

    // Both streams replayed into the live half in timestamp order, exactly
    // as the packets would arrive.
    mark4::LiveAligner aligner;
    std::vector<mark4::AlignedPair> live;
    std::size_t telemetryIndex = 0U;
    std::size_t simRawIndex = 0U;
    while (telemetryIndex < telemetry.size() || simRawIndex < simRaw.size())
    {
        const bool takeTelemetry =
            simRawIndex >= simRaw.size() ||
            (telemetryIndex < telemetry.size() &&
             telemetry[telemetryIndex].timestampUs <= simRaw[simRawIndex].timestampUs);
        if (takeTelemetry)
        {
            aligner.onTelemetry(telemetry[telemetryIndex++]);
        }
        else
        {
            aligner.onSimRaw(simRaw[simRawIndex++]);
        }
        for (const mark4::AlignedPair &pair : aligner.takeDue())
        {
            live.push_back(pair);
        }
    }

    // The live half is always one delay behind: the samples of the last
    // 30 ms are still waiting for an exact state that could turn out nearer.
    // What it did emit has to be the head of what the recording gives, pair
    // for pair, and it has to be all of it but that tail.
    const double cutoff = simRaw.back().timestampUs - mark4::LiveAligner::DELAY_US;
    std::size_t expected = 0U;
    while (expected < offline.size() && offline[expected].timestampUs <= cutoff)
    {
        ++expected;
    }
    REQUIRE(expected > 0U);
    REQUIRE(live.size() == expected);
    for (std::size_t index = 0U; index < live.size(); ++index)
    {
        INFO("pair " << index);
        // Bit-identical, not merely close: the two paths run the same
        // arithmetic on the same values.
        CHECK(live[index].timestampUs == offline[index].timestampUs);
        CHECK(live[index].gapUs == offline[index].gapUs);
        CHECK(live[index].attitudeErrorDeg == offline[index].attitudeErrorDeg);
        CHECK(live[index].altitudeErrorM == offline[index].altitudeErrorM);
        CHECK(live[index].verticalVelocityErrorMps == offline[index].verticalVelocityErrorMps);
    }
}
