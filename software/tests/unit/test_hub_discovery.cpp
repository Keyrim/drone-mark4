/// @file
/// @brief Discovery registry: what the ground side learns from the beacons
///        the transport delivers, and how it tells a restart from a refresh.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

#include "hub/discovery.hpp"
#include "protocol/announce.hpp"

namespace
{
    constexpr std::uint32_t SIM_NODE = 7U;
    constexpr std::uint32_t OTHER_SIM_NODE = 8U;
    constexpr std::uint32_t PLANT_NODE = 9U;

    /// @brief Builds one announce payload, as a beacon carries it: the port
    ///        fields are always 0 since the transport took over.
    /// @param kind announcing process kind
    /// @return the packed payload bytes
    std::array<std::uint8_t, mark4::ANNOUNCE_PACKET_SIZE> announce(mark4::StreamSource kind)
    {
        mark4::AnnouncePacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::ANNOUNCE);
        packet.kind = static_cast<std::uint8_t>(kind);
        packet.sessionId = 0U;
        packet.telemetryPort = 0U;
        packet.commandPort = 0U;
        std::array<std::uint8_t, mark4::ANNOUNCE_PACKET_SIZE> bytes{};
        std::memcpy(bytes.data(), &packet, bytes.size());
        return bytes;
    }
} // namespace

TEST_CASE("a first announce makes a process appear")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM);

    const auto change = registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 1000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::APPEARED);
    CHECK(change->process.kind == mark4::StreamSource::DRONE_SIM);
    CHECK(change->process.nodeId == SIM_NODE);
    CHECK(change->process.lastSeenUs == 1000U);
    CHECK(!(change->process.viaSerial));
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.nodeIdOf(mark4::StreamSource::DRONE_SIM) == SIM_NODE);
}

TEST_CASE("a repeated announce is a silent refresh")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM);
    static_cast<void>(registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 1000U));

    const auto change = registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 2'000'000U);
    CHECK(!(change.has_value()));
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.processes()[0].lastSeenUs == 2'000'000U);
}

TEST_CASE("a new node identity behind the same kind is a restart")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM);
    static_cast<void>(registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 1000U));

    const auto change = registry.onAnnounce(OTHER_SIM_NODE, bytes.data(), bytes.size(), 2000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::RESTARTED);
    CHECK(change->process.nodeId == OTHER_SIM_NODE);
    CHECK(registry.processes().size() == 1U);
    CHECK(registry.nodeIdOf(mark4::StreamSource::DRONE_SIM) == OTHER_SIM_NODE);
}

TEST_CASE("an announce from no node at all is rejected")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM);
    CHECK(!(registry.onAnnounce(0U, bytes.data(), bytes.size(), 1000U).has_value()));
    CHECK(registry.processes().empty());
    CHECK(registry.rejectedAnnounces() == 1U);
}

TEST_CASE("silence makes a process disappear exactly once")
{
    mark4::DiscoveryRegistry registry;
    const auto bytes = announce(mark4::StreamSource::DRONE_SIM);
    static_cast<void>(registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 1000U));

    CHECK(registry.expire(2'000'000U, 3'000'000U).empty());

    const auto gone = registry.expire(4'000'000U, 3'000'000U);
    REQUIRE(gone.size() == 1U);
    CHECK(gone[0].event == mark4::DiscoveryEvent::DISAPPEARED);
    CHECK(gone[0].process.nodeId == SIM_NODE);
    CHECK(registry.processes().empty());

    CHECK(registry.expire(5'000'000U, 3'000'000U).empty());
}

TEST_CASE("two kinds coexist in the registry")
{
    mark4::DiscoveryRegistry registry;
    const auto sim = announce(mark4::StreamSource::DRONE_SIM);
    const auto plant = announce(mark4::StreamSource::SIM_PLANT);

    REQUIRE(registry.onAnnounce(SIM_NODE, sim.data(), sim.size(), 1000U).has_value());
    REQUIRE(registry.onAnnounce(PLANT_NODE, plant.data(), plant.size(), 1000U).has_value());
    REQUIRE(registry.processes().size() == 2U);
    CHECK(registry.nodeIdOf(mark4::StreamSource::DRONE_SIM) == SIM_NODE);
    CHECK(registry.nodeIdOf(mark4::StreamSource::SIM_PLANT) == PLANT_NODE);
    CHECK(registry.nodeIdOf(mark4::StreamSource::FIRMWARE) == 0U);
}

TEST_CASE("serial telemetry synthesizes the firmware entry")
{
    mark4::DiscoveryRegistry registry;

    const auto change = registry.onSerialTelemetry(1000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::APPEARED);
    CHECK(change->process.kind == mark4::StreamSource::FIRMWARE);
    CHECK(change->process.viaSerial);
    CHECK(change->process.nodeId == 0U);

    CHECK(!(registry.onSerialTelemetry(2000U).has_value()));
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.processes()[0].lastSeenUs == 2000U);
    CHECK(registry.nodeIdOf(mark4::StreamSource::FIRMWARE) == 0U);
}

TEST_CASE("an announce on the wrong wire version is counted and dropped")
{
    mark4::DiscoveryRegistry registry;
    auto bytes = announce(mark4::StreamSource::DRONE_SIM);
    bytes[0] = mark4::PROTOCOL_VERSION - 1U;

    CHECK(!(registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size(), 1000U).has_value()));
    CHECK(registry.processes().empty());
    CHECK(registry.rejectedAnnounces() == 1U);

    // A truncated payload is just as invalid, and just as worth counting.
    CHECK(!(registry.onAnnounce(SIM_NODE, bytes.data(), bytes.size() - 1U, 1000U).has_value()));
    CHECK(registry.rejectedAnnounces() == 2U);
}

namespace
{
    /// @brief Feeds one announce text to a directory.
    /// @param directory directory under test
    /// @param text announce payload
    /// @param address address it comes from
    /// @param nowUs current time [us]
    /// @return true when a bridge nobody had seen yet was added
    bool announceBridge(mark4::BridgeDirectory &directory,
                        const std::string &text,
                        const char *address,
                        std::uint64_t nowUs)
    {
        return directory.onAnnounce(address,
                                    47830U,
                                    reinterpret_cast<const std::uint8_t *>(text.data()),
                                    text.size(),
                                    nowUs);
    }
} // namespace

TEST_CASE("a bridge announce is kept once per address and refreshed after that")
{
    mark4::BridgeDirectory directory;

    CHECK(announceBridge(directory, "mark4-bridge c19f6c", "192.168.1.31", 1000U));
    REQUIRE(directory.bridges().size() == 1U);
    CHECK(directory.bridges()[0].address == "192.168.1.31");
    CHECK(directory.bridges()[0].port == 47830U);
    CHECK(directory.bridges()[0].name == "c19f6c");

    // The same bridge announcing again is a refresh, not a second entry.
    CHECK(!announceBridge(directory, "mark4-bridge c19f6c", "192.168.1.31", 2000U));
    REQUIRE(directory.bridges().size() == 1U);
    CHECK(directory.bridges()[0].lastSeenUs == 2000U);

    // A second bridge on the same network is a second entry.
    CHECK(announceBridge(directory, "mark4-bridge aabbcc", "192.168.1.32", 2000U));
    CHECK(directory.bridges().size() == 2U);
}

TEST_CASE("a bridge announce that says anything else is ignored")
{
    mark4::BridgeDirectory directory;

    CHECK(!announceBridge(directory, "", "192.168.1.31", 1000U));
    CHECK(!announceBridge(directory, "hello", "192.168.1.31", 1000U));
    CHECK(!announceBridge(directory, "mark4-bridg", "192.168.1.31", 1000U));
    CHECK(directory.bridges().empty());
}

TEST_CASE("a bridge name is cut down to what a page can display")
{
    mark4::BridgeDirectory directory;

    // The name crosses a trust boundary: it comes from the network and ends
    // up in a web page.
    CHECK(announceBridge(directory, "mark4-bridge <b>x</b> ../\n y", "192.168.1.31", 1000U));
    CHECK(directory.bridges()[0].name == "bxby");

    CHECK(announceBridge(directory, "mark4-bridge 0123456789abcdefghij", "192.168.1.32", 1000U));
    CHECK(directory.bridges()[1].name.size() == mark4::BridgeDirectory::MAX_NAME);
}

TEST_CASE("a bridge that stops announcing is dropped")
{
    mark4::BridgeDirectory directory;
    CHECK(announceBridge(directory, "mark4-bridge c19f6c", "192.168.1.31", 1000U));

    CHECK(directory.expire(2000U, 3000U) == 0U);
    CHECK(directory.bridges().size() == 1U);
    CHECK(directory.expire(4000U, 3000U) == 1U);
    CHECK(directory.bridges().empty());
}
