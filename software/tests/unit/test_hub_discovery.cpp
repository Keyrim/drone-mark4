/// @file
/// @brief Discovery registry: what the ground side learns from the announces
///        the nodes beacon, and how it tells a restart from a refresh.

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <string>

#include "hub/discovery.hpp"
#include "protocol/envelope.hpp"

namespace
{
    constexpr std::uint32_t SIM_NODE = 7U;
    constexpr std::uint32_t OTHER_SIM_NODE = 8U;
    constexpr std::uint32_t PLANT_NODE = 9U;

    /// @brief Builds one announce, on this build's wire.
    /// @param kind announcing node kind
    /// @return the message
    mark4_Announce announce(mark4_NodeKind kind)
    {
        mark4_Announce message = mark4_Announce_init_zero;
        message.kind = kind;
        message.mcu = mark4_Mcu_SIM;
        message.wire_hash = mark4::WIRE_HASH;
        static_cast<void>(std::snprintf(message.name, sizeof(message.name), "%s", "node"));
        return message;
    }
} // namespace

TEST_CASE("a first announce makes a process appear")
{
    mark4::DiscoveryRegistry registry;

    const auto change = registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 1000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::APPEARED);
    CHECK(change->process.kind == mark4_NodeKind_DRONE_SIM);
    CHECK(change->process.nodeId == SIM_NODE);
    CHECK(change->process.lastSeenUs == 1000U);
    CHECK(change->process.name == "node");
    CHECK(change->process.mcu == mark4_Mcu_SIM);
    CHECK(!(change->process.wireMismatch));
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.nodeIdOf(mark4_NodeKind_DRONE_SIM) == SIM_NODE);
    mark4_NodeKind kind = mark4_NodeKind_NODE_KIND_UNSPECIFIED;
    CHECK(registry.kindOf(SIM_NODE, kind));
    CHECK(kind == mark4_NodeKind_DRONE_SIM);
    CHECK(!(registry.kindOf(OTHER_SIM_NODE, kind)));
}

TEST_CASE("a repeated announce is a silent refresh")
{
    mark4::DiscoveryRegistry registry;
    static_cast<void>(registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 1000U));

    const auto change =
        registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 2'000'000U);
    CHECK(!(change.has_value()));
    REQUIRE(registry.processes().size() == 1U);
    CHECK(registry.processes()[0].lastSeenUs == 2'000'000U);
}

TEST_CASE("a new node identity behind the same kind is a restart")
{
    mark4::DiscoveryRegistry registry;
    static_cast<void>(registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 1000U));

    const auto change =
        registry.onAnnounce(OTHER_SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 2000U);
    REQUIRE(change.has_value());
    CHECK(change->event == mark4::DiscoveryEvent::RESTARTED);
    CHECK(change->process.nodeId == OTHER_SIM_NODE);
    CHECK(registry.processes().size() == 1U);
    CHECK(registry.nodeIdOf(mark4_NodeKind_DRONE_SIM) == OTHER_SIM_NODE);
}

TEST_CASE("silence makes a process disappear exactly once")
{
    mark4::DiscoveryRegistry registry;
    static_cast<void>(registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 1000U));

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

    REQUIRE(registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_DRONE_SIM), 1000U).has_value());
    REQUIRE(registry.onAnnounce(PLANT_NODE, announce(mark4_NodeKind_PLANT), 1000U).has_value());
    REQUIRE(registry.processes().size() == 2U);
    CHECK(registry.nodeIdOf(mark4_NodeKind_DRONE_SIM) == SIM_NODE);
    CHECK(registry.nodeIdOf(mark4_NodeKind_PLANT) == PLANT_NODE);
    CHECK(registry.nodeIdOf(mark4_NodeKind_FIRMWARE) == 0U);
}

TEST_CASE("an announce naming no kind is counted and dropped")
{
    mark4::DiscoveryRegistry registry;
    CHECK(!(registry.onAnnounce(SIM_NODE, announce(mark4_NodeKind_NODE_KIND_UNSPECIFIED), 1000U)
                .has_value()));
    CHECK(registry.processes().empty());
    CHECK(registry.rejectedAnnounces() == 1U);
}

TEST_CASE("a node built on another wire schema is listed as a mismatch")
{
    mark4::DiscoveryRegistry registry;
    mark4_Announce foreign = announce(mark4_NodeKind_FIRMWARE);
    foreign.wire_hash = mark4::WIRE_HASH ^ 0x1U;

    const auto change = registry.onAnnounce(0U, foreign, 1000U);
    REQUIRE(change.has_value());
    CHECK(change->process.wireMismatch);
    CHECK(change->process.wireHash == foreign.wire_hash);
    // Still listed: the page must show the board is there and why it is mute.
    REQUIRE(registry.processes().size() == 1U);
}

TEST_CASE("kind names round trip")
{
    for (const mark4_NodeKind kind : {mark4_NodeKind_FIRMWARE,
                                      mark4_NodeKind_DRONE_SIM,
                                      mark4_NodeKind_PLANT,
                                      mark4_NodeKind_GATEWAY,
                                      mark4_NodeKind_BATCH})
    {
        mark4_NodeKind parsed = mark4_NodeKind_NODE_KIND_UNSPECIFIED;
        REQUIRE(mark4::parseNodeKindName(mark4::nodeKindName(kind), parsed));
        CHECK(parsed == kind);
    }
    mark4_NodeKind parsed = mark4_NodeKind_NODE_KIND_UNSPECIFIED;
    CHECK(!(mark4::parseNodeKindName("ghost", parsed)));
    CHECK(std::string(mark4::nodeKindName(mark4_NodeKind_NODE_KIND_UNSPECIFIED)) == "unknown");
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
