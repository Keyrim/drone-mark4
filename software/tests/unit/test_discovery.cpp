/// @file
/// @brief Identity on request over a recording link: a Discovery answers
///        an IdentityRequest with its Announce; a DiscoveryDirectory asks
///        every node that appears, retries on the timeout, gives up, learns
///        the answers, tells its listeners and forgets what the transport
///        expires.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "discovery/discovery.hpp"
#include "discovery/discovery_directory.hpp"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "protocol/wire_hash.hpp"
#include "recording_link.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 10'000'000U;
    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D00U;
    constexpr std::uint32_t NODE_ME = 0xD0000001U;
    constexpr std::uint32_t NODE_PEER = 0xD0000002U;
    constexpr std::uint32_t NODE_OTHER = 0xD0000003U;
    constexpr std::uint64_t TIMEOUT_US = mark4::DiscoveryDirectory::IDENTITY_TIMEOUT_US;

    /// @param kind announced kind
    /// @param name announced name
    /// @param wireHash announced schema hash
    /// @return the announce
    mark4_Announce announceOf(mark4_NodeKind kind,
                              const char *name,
                              std::uint32_t wireHash = mark4::WIRE_HASH)
    {
        mark4_Announce announce = mark4_Announce_init_zero;
        announce.kind = kind;
        announce.mcu = mark4_Mcu_SIM;
        announce.wire_hash = wireHash;
        std::strncpy(announce.name, name, sizeof(announce.name) - 1U);
        return announce;
    }

    /// @param envelope message to encode
    /// @return its bytes
    std::vector<std::uint8_t> encode(const mark4_Envelope &envelope)
    {
        std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE, 0U);
        std::size_t size = 0U;
        REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
        bytes.resize(size);
        return bytes;
    }

    /// @return an IdentityRequest envelope
    mark4_Envelope identityRequest()
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_identity_request_tag;
        return envelope;
    }

    /// @param announce body
    /// @return an Announce envelope
    mark4_Envelope announceEnvelope(const mark4_Announce &announce)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_announce_tag;
        envelope.body.announce = announce;
        return envelope;
    }

    /// @return true when two announces say the same thing
    bool sameAnnounce(const mark4_Announce &a, const mark4_Announce &b)
    {
        return a.kind == b.kind && a.mcu == b.mcu && a.build_epoch == b.build_epoch &&
               a.wire_hash == b.wire_hash && std::strcmp(a.name, b.name) == 0 &&
               std::strcmp(a.git_hash, b.git_hash) == 0;
    }

    /// @brief Counts the recorded frames carrying an Envelope of one tag,
    ///        ignoring the transport's own keepalives (the flagged frames).
    /// @param link link to read
    /// @param tag body tag wanted
    /// @param dst destination the frame must carry
    /// @return frames matching
    std::size_t countSent(const mark4::RecordingLink &link, pb_size_t tag, std::uint32_t dst)
    {
        std::size_t count = 0U;
        for (std::size_t index = 0U; index < link.frames().size(); ++index)
        {
            const mark4::RecordedFrame &frame = link.frames()[index];
            if (frame.header.keepalive || frame.payload.empty() || frame.header.dst != dst)
            {
                continue;
            }
            const std::optional<mark4_Envelope> envelope = link.envelope(index);
            if (envelope.has_value() && envelope->which_body == tag)
            {
                ++count;
            }
        }
        return count;
    }

    /// The listener under test: everything it is told, in order.
    class Spy final : public mark4::AbsDirectoryListener
    {
      public:
        explicit Spy(mark4::DiscoveryDirectory &directory)
            : AbsDirectoryListener(directory)
        {
        }

        void onIdentity(const mark4::DirectoryEntry &entry) override
        {
            identities.push_back(entry);
        }

        void onForgotten(std::uint32_t nodeId) override
        {
            forgotten.push_back(nodeId);
        }

        std::vector<mark4::DirectoryEntry> identities; ///< every onIdentity(), copied
        std::vector<std::uint32_t> forgotten;          ///< every onForgotten()
    };

    /// A node with a directory: link, transport, messenger, directory,
    /// composed in the order a composition composes them.
    struct Bench
    {
        mark4::RecordingLink link;
        mark4::Transport transport{NODE_ME, BOOT_ID};
        mark4::Messenger messenger{transport};
        mark4::DiscoveryDirectory directory{
            messenger, transport, announceOf(mark4_NodeKind_GATEWAY, "me")};

        Bench()
        {
            REQUIRE(transport.addLink(link));
            REQUIRE(transport.init());
            REQUIRE(messenger.init());
            REQUIRE(directory.init());
        }

        /// @brief Makes the transport hear one node once (a keepalive) and
        ///        forgets the keepalive that greets it back.
        /// @param node node that appears
        /// @param nowUs instant of the poll
        void appear(std::uint32_t node, std::uint64_t nowUs)
        {
            link.deliver(node, NODE_ME, {});
            messenger.poll(nowUs);
            link.clear();
        }

        /// @brief Delivers one message from one node and polls once.
        /// @param node sender
        /// @param envelope message
        /// @param nowUs instant of the poll
        void receive(std::uint32_t node, const mark4_Envelope &envelope, std::uint64_t nowUs)
        {
            link.deliver(node, NODE_ME, encode(envelope));
            messenger.poll(nowUs);
        }
    };
} // namespace

TEST_CASE("discovery answers an identity request with its announce", "[discovery]")
{
    mark4::RecordingLink link;
    mark4::Transport transport{NODE_ME, BOOT_ID};
    mark4::Messenger messenger{transport};
    const mark4_Announce self = announceOf(mark4_NodeKind_DRONE_SIM, "drone_sim");
    mark4::Discovery discovery{messenger, self};
    REQUIRE(transport.addLink(link));
    REQUIRE(transport.init());
    REQUIRE(messenger.init());

    link.deliver(NODE_PEER, NODE_ME, encode(identityRequest()));
    messenger.poll(T0_US);

    CHECK(discovery.answered() == 1U);
    CHECK(sameAnnounce(discovery.self(), self));
    REQUIRE(countSent(link, mark4_Envelope_announce_tag, NODE_PEER) == 1U);
    for (std::size_t index = 0U; index < link.frames().size(); ++index)
    {
        const std::optional<mark4_Envelope> envelope = link.envelope(index);
        if (envelope.has_value() && envelope->which_body == mark4_Envelope_announce_tag)
        {
            CHECK(!link.frames()[index].broadcast);
            CHECK(link.frames()[index].header.dst == NODE_PEER);
            CHECK(sameAnnounce(envelope->body.announce, self));
        }
    }
    CHECK(messenger.handled() == 1U);
}

TEST_CASE("directory asks a node that appears, retries on the timeout and gives up", "[discovery]")
{
    Bench bench;
    bench.appear(NODE_PEER, T0_US);
    REQUIRE(bench.directory.size() == 1U);
    CHECK(bench.directory.requests() == 0U); // no instant at node-up: nothing sent yet

    bench.directory.tick(T0_US);
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) == 1U);
    CHECK(bench.directory.requests() == 1U);
    const mark4::DirectoryEntry *entry = bench.directory.find(NODE_PEER);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == mark4::DirectoryEntry::State::PENDING);
    CHECK(entry->requests == 1U);
    CHECK(entry->askedUs == T0_US);

    bench.directory.tick(T0_US + 1U);
    bench.directory.tick(T0_US + TIMEOUT_US - 1U);
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) == 1U);

    bench.directory.tick(T0_US + TIMEOUT_US);
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) == 2U);
    CHECK(bench.directory.requests() == 2U);

    // Every timeout one more, until the retries are spent.
    for (std::uint8_t request = 3U; request <= mark4::DiscoveryDirectory::IDENTITY_RETRIES;
         ++request)
    {
        bench.directory.tick(T0_US + TIMEOUT_US * (request - 1U));
    }
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) ==
          mark4::DiscoveryDirectory::IDENTITY_RETRIES);
    CHECK(entry->state == mark4::DirectoryEntry::State::PENDING);
    CHECK(bench.directory.muted() == 0U);

    const std::uint64_t giveUpUs = T0_US + TIMEOUT_US * mark4::DiscoveryDirectory::IDENTITY_RETRIES;
    bench.directory.tick(giveUpUs);
    CHECK(entry->state == mark4::DirectoryEntry::State::MUTE);
    CHECK(entry->updatedUs == giveUpUs);
    CHECK(bench.directory.muted() == 1U);
    bench.directory.tick(giveUpUs + TIMEOUT_US);
    bench.directory.tick(giveUpUs + TIMEOUT_US * 4U);
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) ==
          mark4::DiscoveryDirectory::IDENTITY_RETRIES);
    CHECK(bench.directory.requests() == mark4::DiscoveryDirectory::IDENTITY_RETRIES);
}

TEST_CASE("directory learns an announce and tells its listener of every change", "[discovery]")
{
    Bench bench;
    Spy spy(bench.directory);
    bench.appear(NODE_PEER, T0_US);
    bench.directory.tick(T0_US);

    const mark4_Announce first = announceOf(mark4_NodeKind_DRONE_SIM, "drone_sim");
    bench.receive(NODE_PEER, announceEnvelope(first), T0_US + 1U);
    const mark4::DirectoryEntry *entry = bench.directory.find(NODE_PEER);
    REQUIRE(entry != nullptr);
    CHECK(entry->state == mark4::DirectoryEntry::State::KNOWN);
    CHECK(!entry->wireMismatch);
    CHECK(entry->updatedUs == T0_US + 1U);
    CHECK(sameAnnounce(entry->announce, first));
    CHECK(bench.directory.learnt() == 1U);
    REQUIRE(spy.identities.size() == 1U);
    CHECK(spy.identities[0].id == NODE_PEER);
    CHECK(sameAnnounce(spy.identities[0].announce, first));

    // The same answer again: stored, not announced to the listener.
    bench.receive(NODE_PEER, announceEnvelope(first), T0_US + 2U);
    CHECK(bench.directory.learnt() == 2U);
    CHECK(spy.identities.size() == 1U);
    CHECK(entry->updatedUs == T0_US + 1U);

    // A different name: the node changed, the listener hears it.
    bench.receive(
        NODE_PEER, announceEnvelope(announceOf(mark4_NodeKind_DRONE_SIM, "renamed")), T0_US + 3U);
    REQUIRE(spy.identities.size() == 2U);
    CHECK(std::strcmp(spy.identities[1].announce.name, "renamed") == 0);
    CHECK(entry->updatedUs == T0_US + 3U);

    // Another schema: still KNOWN, flagged.
    bench.receive(
        NODE_PEER,
        announceEnvelope(announceOf(mark4_NodeKind_DRONE_SIM, "renamed", mark4::WIRE_HASH ^ 1U)),
        T0_US + 4U);
    CHECK(entry->state == mark4::DirectoryEntry::State::KNOWN);
    CHECK(entry->wireMismatch);
    REQUIRE(spy.identities.size() == 3U);
    CHECK(spy.identities[2].wireMismatch);

    // No further request for a node that answered.
    bench.link.clear();
    bench.directory.tick(T0_US + TIMEOUT_US * 2U);
    CHECK(countSent(bench.link, mark4_Envelope_identity_request_tag, NODE_PEER) == 0U);
}

TEST_CASE("directory lists the known nodes of the wanted kinds", "[discovery]")
{
    Bench bench;
    constexpr std::uint32_t NODE_PLANT = 0xD0000004U;
    bench.appear(NODE_PEER, T0_US);
    bench.appear(NODE_OTHER, T0_US);
    bench.appear(NODE_PLANT, T0_US);
    bench.directory.tick(T0_US);
    REQUIRE(bench.directory.size() == 3U);

    // NODE_PEER answers as a drone, NODE_PLANT as a plant, NODE_OTHER never.
    bench.receive(NODE_PEER, announceEnvelope(announceOf(mark4_NodeKind_DRONE_SIM, "sim")), T0_US);
    bench.receive(NODE_PLANT, announceEnvelope(announceOf(mark4_NodeKind_PLANT, "godot")), T0_US);
    for (std::uint8_t timeout = 1U; timeout <= mark4::DiscoveryDirectory::IDENTITY_RETRIES;
         ++timeout)
    {
        bench.directory.tick(T0_US + TIMEOUT_US * timeout);
    }
    REQUIRE(bench.directory.find(NODE_OTHER) != nullptr);
    CHECK(bench.directory.find(NODE_OTHER)->state == mark4::DirectoryEntry::State::MUTE);

    constexpr std::array<mark4_NodeKind, 2> DRONE_KINDS = {mark4_NodeKind_DRONE_SIM,
                                                           mark4_NodeKind_FIRMWARE};
    std::array<mark4::DirectoryEntry, mark4::DiscoveryDirectory::MAX_ENTRIES> out{};
    const std::size_t drones = bench.directory.nodesOfKind(DRONE_KINDS, out);
    REQUIRE(drones == 1U);
    CHECK(out[0].id == NODE_PEER);
    CHECK(out[0].state == mark4::DirectoryEntry::State::KNOWN);

    constexpr std::array<mark4_NodeKind, 1> PLANT_KIND = {mark4_NodeKind_PLANT};
    CHECK(bench.directory.nodesOfKind(PLANT_KIND, out) == 1U);
    CHECK(out[0].id == NODE_PLANT);

    // A too small output is filled and stops.
    constexpr std::array<mark4_NodeKind, 2> BOTH = {mark4_NodeKind_DRONE_SIM, mark4_NodeKind_PLANT};
    std::array<mark4::DirectoryEntry, 1> one{};
    CHECK(bench.directory.nodesOfKind(BOTH, one) == 1U);
    CHECK(bench.directory.nodesOfKind(BOTH, out) == 2U);

    // find(), size() and entry(i) agree.
    std::size_t found = 0U;
    for (std::size_t index = 0U; index < bench.directory.size(); ++index)
    {
        const mark4::DirectoryEntry &entry = bench.directory.entry(index);
        CHECK(bench.directory.find(entry.id) == &entry);
        ++found;
    }
    CHECK(found == 3U);
    CHECK(bench.directory.find(NODE_ME) == nullptr);
}

TEST_CASE("directory forgets a node the transport expires", "[discovery]")
{
    Bench bench;
    Spy spy(bench.directory);
    bench.appear(NODE_PEER, T0_US);
    bench.appear(NODE_OTHER, T0_US);
    bench.directory.tick(T0_US);
    bench.receive(NODE_PEER, announceEnvelope(announceOf(mark4_NodeKind_DRONE_SIM, "sim")), T0_US);
    REQUIRE(bench.directory.size() == 2U);

    // NODE_OTHER keeps talking, NODE_PEER falls silent.
    const std::uint64_t laterUs = T0_US + mark4::Transport::NODE_EXPIRY_US;
    bench.appear(NODE_OTHER, laterUs - 1U);
    bench.messenger.poll(laterUs);

    CHECK(!bench.transport.isAlive(NODE_PEER));
    CHECK(bench.directory.find(NODE_PEER) == nullptr);
    REQUIRE(bench.directory.size() == 1U);
    CHECK(bench.directory.entry(0).id == NODE_OTHER);
    REQUIRE(spy.forgotten.size() == 1U);
    CHECK(spy.forgotten[0] == NODE_PEER);
    CHECK(spy.identities.size() == 1U);
}

TEST_CASE("directory still answers an identity request itself", "[discovery]")
{
    Bench bench;
    bench.appear(NODE_PEER, T0_US);
    bench.receive(NODE_PEER, identityRequest(), T0_US);

    CHECK(bench.directory.answered() == 1U);
    REQUIRE(countSent(bench.link, mark4_Envelope_announce_tag, NODE_PEER) == 1U);
    for (std::size_t index = 0U; index < bench.link.frames().size(); ++index)
    {
        const std::optional<mark4_Envelope> envelope = bench.link.envelope(index);
        if (envelope.has_value() && envelope->which_body == mark4_Envelope_announce_tag)
        {
            CHECK(envelope->body.announce.kind == mark4_NodeKind_GATEWAY);
            CHECK(std::strcmp(envelope->body.announce.name, "me") == 0);
        }
    }
    CHECK(bench.messenger.handled() == 1U);
}

TEST_CASE("directory init fails with one listener too many", "[discovery]")
{
    Bench bench;
    std::array<std::optional<Spy>, mark4::DiscoveryDirectory::MAX_LISTENERS> four;
    for (std::optional<Spy> &spy : four)
    {
        spy.emplace(bench.directory);
    }
    CHECK(bench.directory.init());
    {
        Spy fifth(bench.directory);
        CHECK(!bench.directory.init());
    }
    CHECK(bench.directory.init());
}
