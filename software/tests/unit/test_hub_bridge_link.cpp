/// @file
/// @brief The board's link as the hub sees it: a UartLink over a datagram
///        byte stream, through a bridge that forwards datagrams unchanged
///        and learns each peer from the first datagram it receives. Both
///        ends of the bridge are driven here, so the test is the whole path
///        hub -> bridge -> board and back, minus the UART.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include <time.h>

#include "fake_bridge.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"
#include "transport/udp_byte_stream.hpp"

namespace
{
    constexpr std::uint32_t HUB_NODE = 0x4B000001U;
    constexpr std::uint32_t BOARD_NODE = 0xB0A4D001U;
    constexpr std::uint64_t T0_US = 5'000'000U;

    /// Attempts a loopback delivery is given before the test gives up.
    constexpr int ATTEMPTS = 200;

    /// @brief Lets the loopback breathe between two polls.
    void breathe()
    {
        const timespec request = {0, 1'000'000L};
        static_cast<void>(::nanosleep(&request, nullptr));
    }

    /// @brief Pumps the bridge and polls one link until it hands out a frame.
    /// @return the frame, empty when none came within the attempts
    std::vector<std::uint8_t> receiveThrough(mark4::FakeBridge &bridge, mark4::UartLink &link)
    {
        std::array<std::uint8_t, mark4::MAX_FRAME_SIZE> buffer{};
        for (int attempt = 0; attempt < ATTEMPTS; ++attempt)
        {
            bridge.pump();
            mark4::LinkAddress from;
            const std::size_t size = link.receive(buffer.data(), buffer.size(), from);
            if (size > 0U)
            {
                return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)};
            }
            breathe();
        }
        return {};
    }

    /// What one end observed from its transport.
    struct Seen
    {
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> delivered;

        static void Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size)
        {
            static_cast<Seen *>(context)->delivered.emplace_back(
                src, std::vector<std::uint8_t>(payload, payload + size));
        }
    };
} // namespace

TEST_CASE("a uart link over datagrams crosses the bridge in both directions", "[udp]")
{
    mark4::FakeBridge bridge;
    REQUIRE(bridge.ok());

    mark4::UdpByteStream hubStream;
    mark4::UdpByteStream boardStream;
    REQUIRE(hubStream.open("127.0.0.1", bridge.groundPort()));
    REQUIRE(boardStream.open("127.0.0.1", bridge.boardPort()));
    REQUIRE(hubStream.isOpen());
    mark4::UartLink hubLink(hubStream);
    mark4::UartLink boardLink(boardStream);

    // Any datagram teaches the bridge who its peers are: the first one from
    // each side goes nowhere (the other peer is unknown yet) and that is all
    // it costs. No hello byte, no address configured anywhere.
    const std::vector<std::uint8_t> knock = {0x01U};
    REQUIRE(hubLink.broadcast(knock.data(), knock.size()));
    bridge.pump();
    REQUIRE(boardLink.broadcast(knock.data(), knock.size()));
    CHECK(receiveThrough(bridge, hubLink) == knock);

    // A frame longer than what the link reads per call: the stream hands one
    // datagram out over several reads and the framing sees every byte.
    std::vector<std::uint8_t> big(300U);
    std::iota(big.begin(), big.end(), std::uint8_t{0U});
    REQUIRE(hubLink.send(big.data(), big.size(), mark4::LinkAddress{}));
    CHECK(receiveThrough(bridge, boardLink) == big);
    REQUIRE(boardLink.send(big.data(), big.size(), mark4::LinkAddress{}));
    CHECK(receiveThrough(bridge, hubLink) == big);

    // Closed, the stream reads nothing and refuses writes: the transport
    // then counts a drop instead of blocking.
    hubStream.close();
    CHECK(!hubStream.isOpen());
    CHECK(!hubLink.send(knock.data(), knock.size(), mark4::LinkAddress{}));
    std::array<std::uint8_t, 8U> scratch{};
    CHECK(hubStream.read(scratch.data(), scratch.size()) == 0U);
}

TEST_CASE("a malformed bridge address is refused")
{
    mark4::UdpByteStream stream;
    CHECK(!stream.open("not-an-address", 47830U));
    CHECK(!stream.open("127.0.0.1", 0U));
    CHECK(!stream.open(nullptr, 47830U));
    CHECK(!stream.isOpen());
}

TEST_CASE("two transport nodes find each other through the bridge and unicast", "[udp]")
{
    mark4::FakeBridge bridge;
    REQUIRE(bridge.ok());
    mark4::UdpByteStream hubStream;
    mark4::UdpByteStream boardStream;
    REQUIRE(hubStream.open("127.0.0.1", bridge.groundPort()));
    REQUIRE(boardStream.open("127.0.0.1", bridge.boardPort()));
    mark4::UartLink hubLink(hubStream);
    mark4::UartLink boardLink(boardStream);

    mark4::Transport hub(HUB_NODE);
    mark4::Transport board(BOARD_NODE);
    REQUIRE(hub.addLink(hubLink));
    REQUIRE(board.addLink(boardLink));
    REQUIRE(hub.init());
    REQUIRE(board.init());
    const std::vector<std::uint8_t> hubBeacon = {'h', 'u', 'b'};
    const std::vector<std::uint8_t> boardBeacon = {'f', 'c'};
    hub.setBeacon(hubBeacon.data(), hubBeacon.size());
    board.setBeacon(boardBeacon.data(), boardBeacon.size());

    // The beacons are what teach the bridge and what make each node learn
    // the other; the first poll sends them.
    Seen hubSeen;
    Seen boardSeen;
    std::uint64_t nowUs = T0_US;
    for (int attempt = 0;
         attempt < ATTEMPTS && !(hub.isAlive(BOARD_NODE) && board.isAlive(HUB_NODE));
         ++attempt)
    {
        // Every attempt is a whole beacon period so a beacon lost to a peer
        // not learnt yet is followed by another.
        nowUs += mark4::Transport::BEACON_PERIOD_US;
        hub.poll(nowUs, &Seen::Deliver, &hubSeen);
        bridge.pump();
        board.poll(nowUs, &Seen::Deliver, &boardSeen);
        bridge.pump();
        breathe();
    }
    REQUIRE(hub.isAlive(BOARD_NODE));
    REQUIRE(board.isAlive(HUB_NODE));
    // Let the beacons still in flight (the one each side unicasts to a
    // newcomer) land before counting deliveries.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        bridge.pump();
        hub.poll(nowUs, &Seen::Deliver, &hubSeen);
        board.poll(nowUs, &Seen::Deliver, &boardSeen);
        breathe();
    }

    // A command goes to the board by node id, on the one link it has.
    const std::vector<std::uint8_t> command = {'r', 'c'};
    REQUIRE(hub.send(BOARD_NODE, command.data(), command.size()));
    std::size_t before = boardSeen.delivered.size();
    for (int attempt = 0; attempt < ATTEMPTS && boardSeen.delivered.size() == before; ++attempt)
    {
        bridge.pump();
        board.poll(nowUs, &Seen::Deliver, &boardSeen);
        breathe();
    }
    REQUIRE(boardSeen.delivered.size() == before + 1U);
    CHECK(boardSeen.delivered.back().first == HUB_NODE);
    CHECK(boardSeen.delivered.back().second == command);

    // The board's answer is a broadcast, like its telemetry.
    const std::vector<std::uint8_t> answer = {'a', 'c', 'k'};
    REQUIRE(board.send(mark4::BROADCAST_NODE, answer.data(), answer.size()));
    before = hubSeen.delivered.size();
    for (int attempt = 0; attempt < ATTEMPTS && hubSeen.delivered.size() == before; ++attempt)
    {
        bridge.pump();
        hub.poll(nowUs, &Seen::Deliver, &hubSeen);
        breathe();
    }
    REQUIRE(hubSeen.delivered.size() == before + 1U);
    CHECK(hubSeen.delivered.back().first == BOARD_NODE);
    CHECK(hubSeen.delivered.back().second == answer);
}
