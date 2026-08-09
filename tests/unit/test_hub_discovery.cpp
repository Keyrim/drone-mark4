/// @file
/// @brief Discovery registry: what the ground side learns from the announce
///        broadcast, and how it tells a restart from a refresh.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "hub/discovery.hpp"
#include "protocol/announce.hpp"

namespace
{
    /// @brief Builds one announce datagram.
    /// @param kind announcing process kind
    /// @param sessionId session identity, 0 when the process assigns none
    /// @param telemetryPort telemetry broadcast port
    /// @param commandPort command listen port
    /// @return the packed datagram bytes
    std::array<std::uint8_t, mark4::ANNOUNCE_PACKET_SIZE> announce(mark4::StreamSource kind,
                                                                   std::uint32_t sessionId,
                                                                   std::uint16_t telemetryPort,
                                                                   std::uint16_t commandPort)
    {
        mark4::AnnouncePacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::ANNOUNCE);
        packet.kind = static_cast<std::uint8_t>(kind);
        packet.sessionId = sessionId;
        packet.telemetryPort = telemetryPort;
        packet.commandPort = commandPort;
        std::array<std::uint8_t, mark4::ANNOUNCE_PACKET_SIZE> bytes{};
        std::memcpy(bytes.data(), &packet, bytes.size());
        return bytes;
    }
} // namespace

TEST_CASE("a first announce makes a process appear")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);

    const auto change = registry.onAnnounce(bytes.data(), bytes.size(), 1000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::APPEARED);
    CHECK(change->process.kind == mark4::StreamSource::DRONE_SIM);
    CHECK(change->process.sessionId == 7U);
    CHECK(change->process.telemetryPort == 47801U);
    CHECK(change->process.commandPort == 47804U);
    CHECK(change->process.lastSeenUs == 1000U);
    CHECK_FALSE(change->process.viaSerial);
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.commandPortOf(mark4::StreamSource::DRONE_SIM) == 47804U);
}

TEST_CASE("a repeated announce is a silent refresh")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);
    static_cast<void>(registry.onAnnounce(bytes.data(), bytes.size(), 1000U));

    const auto change = registry.onAnnounce(bytes.data(), bytes.size(), 2'000'000U);
    CHECK_FALSE(change.has_value());
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.processes()[0].lastSeenUs == 2'000'000U);
}

TEST_CASE("a new session identity behind the same ports is a restart")
{
    mark4::DiscoveryRegistry registry;
    const auto first = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);
    const auto second = announce(mark4::StreamSource::DRONE_SIM, 8U, 47801U, 47804U);
    static_cast<void>(registry.onAnnounce(first.data(), first.size(), 1000U));

    const auto change = registry.onAnnounce(second.data(), second.size(), 2000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::RESTARTED);
    CHECK(change->process.sessionId == 8U);
    CHECK(registry.processes().size() == 1U);
}

TEST_CASE("an unassigned session identity never reports a restart")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM, 0U, 47801U, 47804U);
    REQUIRE(registry.onAnnounce(bytes.data(), bytes.size(), 1000U).has_value());

    CHECK_FALSE(registry.onAnnounce(bytes.data(), bytes.size(), 2000U).has_value());
    CHECK(registry.processes()[0].lastSeenUs == 2000U);

    // Even coming back from a real restart, a process that assigns no
    // identity can only ever look like the same one refreshing itself.
    CHECK_FALSE(registry.onAnnounce(bytes.data(), bytes.size(), 3000U).has_value());
}

TEST_CASE("silence makes a process disappear exactly once")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);
    static_cast<void>(registry.onAnnounce(bytes.data(), bytes.size(), 1000U));

    CHECK(registry.expire(2'000'000U, 3'000'000U).empty());

    const auto gone = registry.expire(4'000'000U, 3'000'000U);
    REQUIRE(gone.size() == 1U);
    CHECK(gone[0].event == mark4::DiscoveryEvent::DISAPPEARED);
    CHECK(gone[0].process.telemetryPort == 47801U);
    CHECK(registry.processes().empty());

    CHECK(registry.expire(5'000'000U, 3'000'000U).empty());
}

TEST_CASE("two kinds coexist in the registry")
{
    mark4::DiscoveryRegistry registry;
    const auto sim = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);
    const auto plant = announce(mark4::StreamSource::SIM_PLANT, 9U, 47802U, 47800U);

    REQUIRE(registry.onAnnounce(sim.data(), sim.size(), 1000U).has_value());
    REQUIRE(registry.onAnnounce(plant.data(), plant.size(), 1000U).has_value());
    REQUIRE(registry.processes().size() == 2U);
    CHECK(registry.commandPortOf(mark4::StreamSource::DRONE_SIM) == 47804U);
    CHECK(registry.commandPortOf(mark4::StreamSource::SIM_PLANT) == 47800U);
    CHECK(registry.commandPortOf(mark4::StreamSource::FIRMWARE) == 0U);
}

TEST_CASE("serial telemetry synthesizes the firmware entry")
{
    mark4::DiscoveryRegistry registry;

    const auto change = registry.onSerialTelemetry(1000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::APPEARED);
    CHECK(change->process.kind == mark4::StreamSource::FIRMWARE);
    CHECK(change->process.viaSerial);
    CHECK(change->process.telemetryPort == 0U);
    CHECK(change->process.commandPort == 0U);

    CHECK_FALSE(registry.onSerialTelemetry(2000U).has_value());
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.processes()[0].lastSeenUs == 2000U);
}

TEST_CASE("an announce on the wrong wire version is counted and dropped")
{
    mark4::DiscoveryRegistry registry;
    auto bytes = announce(mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U);
    bytes[0] = mark4::PROTOCOL_VERSION - 1U;

    CHECK_FALSE(registry.onAnnounce(bytes.data(), bytes.size(), 1000U).has_value());
    CHECK(registry.processes().empty());
    CHECK(registry.rejectedAnnounces() == 1U);

    // A truncated datagram is just as invalid, and just as worth counting.
    CHECK_FALSE(registry.onAnnounce(bytes.data(), bytes.size() - 1U, 1000U).has_value());
    CHECK(registry.rejectedAnnounces() == 2U);
}
