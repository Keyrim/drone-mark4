/// @file
/// @brief Sequence health of the decoded streams: what the numbering says
///        about a link the hub cannot otherwise question.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "hub/stream_health.hpp"

TEST_CASE("an unbroken run of numbers loses nothing")
{
    mark4::StreamHealth health;
    for (std::uint16_t sequence = 0U; sequence < 10U; ++sequence)
    {
        health.onPacket(mark4::StreamKind::TELEMETRY, 2U, sequence);
    }

    REQUIRE(health.links().size() == 1U);
    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.stream == mark4::StreamKind::TELEMETRY);
    CHECK(link.sourceId == 2U);
    CHECK(link.received == 10U);
    CHECK(link.lost == 0U);
    CHECK(link.duplicates == 0U);
    CHECK(link.resyncs == 0U);
    CHECK(link.lastSequence == 9U);
    CHECK(mark4::linkLossRate(link) == Catch::Approx(0.0));
}

TEST_CASE("a gap in the numbering counts the packets it swallowed")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 100U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 104U);

    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.received == 2U);
    CHECK(link.lost == 3U);
    CHECK(link.resyncs == 0U);
    CHECK(link.lastSequence == 104U);
    CHECK(mark4::linkLossRate(link) == Catch::Approx(0.6));
}

TEST_CASE("the numbering wraps without inventing a loss")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::SIM_RAW, 4U, 65534U);
    health.onPacket(mark4::StreamKind::SIM_RAW, 4U, 65535U);
    health.onPacket(mark4::StreamKind::SIM_RAW, 4U, 0U);
    health.onPacket(mark4::StreamKind::SIM_RAW, 4U, 1U);

    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.received == 4U);
    CHECK(link.lost == 0U);
    CHECK(link.resyncs == 0U);
    CHECK(link.lastSequence == 1U);
}

TEST_CASE("a jump past the threshold is one resync, not a flood of losses")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 10U);
    // A sender that restarted: the number goes back, which in 16-bit
    // arithmetic is a forward jump of nearly the whole range.
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 0U);
    // And a forward jump larger than the threshold, the hub joining a stream
    // that has been running for a while.
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 5000U);

    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.received == 3U);
    CHECK(link.lost == 0U);
    CHECK(link.resyncs == 2U);
    CHECK(link.lastSequence == 5000U);
}

TEST_CASE("a gap exactly at the threshold is still a loss")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 0U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, mark4::StreamHealth::RESYNC_THRESHOLD);

    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.lost == mark4::StreamHealth::RESYNC_THRESHOLD - 1U);
    CHECK(link.resyncs == 0U);
}

TEST_CASE("a repeated number is a duplicate and moves nothing")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 7U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 7U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 7U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 8U);

    const mark4::LinkHealth &link = health.links()[0];
    CHECK(link.received == 4U);
    CHECK(link.duplicates == 2U);
    CHECK(link.lost == 0U);
    CHECK(link.lastSequence == 8U);
}

TEST_CASE("every stream and source pair is counted on its own")
{
    mark4::StreamHealth health;
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 0U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 1U, 0U);
    health.onPacket(mark4::StreamKind::SIM_RAW, 2U, 0U);
    health.onPacket(mark4::StreamKind::TELEMETRY, 2U, 2U);

    REQUIRE(health.links().size() == 3U);
    CHECK(health.links()[0].lost == 1U);
    CHECK(health.links()[1].lost == 0U);
    CHECK(health.links()[2].stream == mark4::StreamKind::SIM_RAW);
    CHECK(std::string(mark4::streamKindName(mark4::StreamKind::SIM_RAW)) == "simRaw");
    CHECK(std::string(mark4::streamKindName(mark4::StreamKind::TELEMETRY)) == "telemetry");
}
