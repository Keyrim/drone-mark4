/// @file
/// @brief The command ring: order, capacity, and the origin node it keeps
///        next to every payload so an answer can be addressed back.

#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "platform_common/command_receiver_transport.hpp"
#include "transport/frame.hpp"

namespace
{
    constexpr std::uint32_t NODE_ONE = 0x11111111U;
    constexpr std::uint32_t NODE_TWO = 0x22222222U;

    /// @brief Queues one single-byte payload.
    /// @param ring ring to push into
    /// @param src origin node of the payload
    /// @param marker the payload's only byte
    void pushOne(mark4::CommandReceiverTransport &ring, std::uint32_t src, std::uint8_t marker)
    {
        const std::array<std::uint8_t, 1U> payload{marker};
        ring.push(src, payload.data(), payload.size());
    }
} // namespace

TEST_CASE("the command ring hands out payloads in order with the node they came from")
{
    mark4::CommandReceiverTransport ring;
    pushOne(ring, NODE_ONE, 0xA1U);
    pushOne(ring, NODE_TWO, 0xB2U);

    std::array<std::uint8_t, 4U> buffer{};
    std::uint32_t src = 0U;

    REQUIRE(ring.poll(buffer.data(), buffer.size(), src) == 1U);
    REQUIRE(buffer[0] == 0xA1U);
    REQUIRE(src == NODE_ONE);

    REQUIRE(ring.poll(buffer.data(), buffer.size(), src) == 1U);
    REQUIRE(buffer[0] == 0xB2U);
    REQUIRE(src == NODE_TWO);

    REQUIRE(ring.poll(buffer.data(), buffer.size(), src) == 0U);
    REQUIRE(ring.packetsReceived() == 2U);
    REQUIRE(ring.dropped() == 0U);
}

TEST_CASE("a burst beyond the ring drops the oldest, origins included")
{
    mark4::CommandReceiverTransport ring;
    // One more than the ring holds: the first payload and its origin are the
    // ones that go.
    for (std::size_t index = 0U; index <= mark4::CommandReceiverTransport::CAPACITY; ++index)
    {
        pushOne(ring, index == 0U ? NODE_ONE : NODE_TWO, static_cast<std::uint8_t>(index));
    }

    std::array<std::uint8_t, 4U> buffer{};
    std::uint32_t src = 0U;
    REQUIRE(ring.poll(buffer.data(), buffer.size(), src) == 1U);
    REQUIRE(buffer[0] == 1U);
    REQUIRE(src == NODE_TWO);
    REQUIRE(ring.dropped() == 1U);
}

TEST_CASE("a payload too large for the caller's buffer is dropped, not truncated")
{
    mark4::CommandReceiverTransport ring;
    const std::array<std::uint8_t, 8U> payload{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    ring.push(NODE_ONE, payload.data(), payload.size());

    std::array<std::uint8_t, 4U> tooSmall{};
    std::uint32_t src = 0U;
    REQUIRE(ring.poll(tooSmall.data(), tooSmall.size(), src) == 0U);
    REQUIRE(ring.dropped() == 1U);
    // The slot is consumed all the same: a payload nobody can read must not
    // wedge the ring behind it.
    REQUIRE(ring.poll(tooSmall.data(), tooSmall.size(), src) == 0U);
}
