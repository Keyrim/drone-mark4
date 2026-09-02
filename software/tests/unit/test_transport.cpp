/// @file
/// @brief Transport core over fake links: header codec, node table, beacon,
///        sequence accounting, relay and its outbound filter; the UART link
///        over an in-memory byte pipe; the UDP link between two nodes on one
///        host.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include <unistd.h>

#include "byte_pipe.hpp"
#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/node_id.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"
#include "transport/udp_link.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 10'000'000U;
    constexpr std::uint32_t NODE_A = 0xA0000001U;
    constexpr std::uint32_t NODE_B = 0xB0000002U;
    constexpr std::uint32_t NODE_C = 0xC0000003U;

    /// One captured frame with the link address it was for.
    struct Datagram
    {
        std::vector<std::uint8_t> bytes; ///< frame bytes
        mark4::LinkAddress from;         ///< sender's address on the bus
        bool broadcast = false;          ///< sent to everyone
    };

    /// Virtual medium: every FakeLink attached to it is a peer, addressed by
    /// its attach index. Allocates freely: this is a test.
    class FakeBus
    {
      public:
        std::vector<std::deque<Datagram>> inbox; ///< one queue per attached link
    };

    /// One endpoint on a FakeBus.
    class FakeLink final : public mark4::AbsLink
    {
      public:
        explicit FakeLink(FakeBus &bus)
            : m_bus(bus),
              m_index(static_cast<std::uint32_t>(bus.inbox.size()))
        {
            bus.inbox.emplace_back();
        }

        bool send(const std::uint8_t *data,
                  std::size_t size,
                  const mark4::LinkAddress &address) override
        {
            ++m_sent;
            if (address.host >= m_bus.inbox.size())
            {
                return false;
            }
            m_bus.inbox[address.host].push_back(
                Datagram{std::vector<std::uint8_t>(data, data + size), self(), false});
            return true;
        }

        bool broadcast(const std::uint8_t *data, std::size_t size) override
        {
            ++m_broadcasts;
            for (std::deque<Datagram> &queue : m_bus.inbox)
            {
                // A real broadcast medium hands the frame back to its sender
                // too; the transport must cope with its own echo.
                queue.push_back(
                    Datagram{std::vector<std::uint8_t>(data, data + size), self(), true});
            }
            return true;
        }

        std::size_t receive(std::uint8_t *bufferOut,
                            std::size_t capacity,
                            mark4::LinkAddress &fromOut) override
        {
            std::deque<Datagram> &queue = m_bus.inbox[m_index];
            if (queue.empty())
            {
                return 0U;
            }
            const Datagram datagram = queue.front();
            queue.pop_front();
            if (datagram.bytes.size() > capacity)
            {
                return 0U;
            }
            std::memcpy(bufferOut, datagram.bytes.data(), datagram.bytes.size());
            fromOut = datagram.from;
            return datagram.bytes.size();
        }

        [[nodiscard]] std::uint32_t sent() const
        {
            return m_sent;
        }

        [[nodiscard]] std::uint32_t broadcasts() const
        {
            return m_broadcasts;
        }

      private:
        [[nodiscard]] mark4::LinkAddress self() const
        {
            return mark4::LinkAddress{m_index, 1U};
        }

        FakeBus &m_bus;                  ///< the medium
        std::uint32_t m_index;           ///< this endpoint's address on it
        std::uint32_t m_sent = 0U;       ///< unicast frames sent
        std::uint32_t m_broadcasts = 0U; ///< broadcast frames sent
    };

    /// Everything an application would observe from one transport.
    struct Observer
    {
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>
            delivered;                   ///< (src, payload)
        std::vector<std::uint32_t> up;   ///< nodes that appeared, in order
        std::vector<std::uint32_t> down; ///< nodes that expired, in order

        static void Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size)
        {
            static_cast<Observer *>(context)->delivered.emplace_back(
                src, std::vector<std::uint8_t>(payload, payload + size));
        }

        static void Up(void *context, const mark4::Transport::Node &node)
        {
            static_cast<Observer *>(context)->up.push_back(node.id);
        }

        static void Down(void *context, const mark4::Transport::Node &node)
        {
            static_cast<Observer *>(context)->down.push_back(node.id);
        }

        void attach(mark4::Transport &transport)
        {
            transport.setNodeCallbacks(&Observer::Up, &Observer::Down, this);
        }

        void poll(mark4::Transport &transport, std::uint64_t nowUs)
        {
            transport.poll(nowUs, &Observer::Deliver, this);
        }
    };

    const std::vector<std::uint8_t> HELLO = {'h', 'e', 'l', 'l', 'o'};
    const std::vector<std::uint8_t> BEACON_A = {0xAA, 0x01};
    const std::vector<std::uint8_t> BEACON_B = {0xBB, 0x02};

    /// @brief Builds one raw frame the way a foreign sender would.
    std::vector<std::uint8_t> rawFrame(std::uint32_t src,
                                       std::uint32_t dst,
                                       std::uint16_t seq,
                                       const std::vector<std::uint8_t> &payload)
    {
        mark4::FrameHeader header;
        header.src = src;
        header.dst = dst;
        header.seq = seq;
        header.hops = mark4::Transport::INITIAL_HOPS;
        std::vector<std::uint8_t> frame(mark4::FRAME_HEADER_SIZE + payload.size());
        mark4::encodeFrameHeader(header, frame.data());
        std::memcpy(frame.data() + mark4::FRAME_HEADER_SIZE, payload.data(), payload.size());
        return frame;
    }

    /// @brief Pushes one raw frame into a bus endpoint's inbox.
    void inject(FakeBus &bus,
                std::size_t endpoint,
                std::uint32_t fromEndpoint,
                const std::vector<std::uint8_t> &frame)
    {
        bus.inbox[endpoint].push_back(Datagram{frame, mark4::LinkAddress{fromEndpoint, 1U}, false});
    }
} // namespace

TEST_CASE("frame header round trips little-endian")
{
    mark4::FrameHeader header;
    header.src = 0x01020304U;
    header.dst = 0xA0B0C0D0U;
    header.seq = 0xBEEFU;
    header.hops = 3U;
    std::array<std::uint8_t, mark4::FRAME_HEADER_SIZE> bytes{};
    mark4::encodeFrameHeader(header, bytes.data());

    const std::array<std::uint8_t, mark4::FRAME_HEADER_SIZE> expected = {
        0x04, 0x03, 0x02, 0x01, 0xD0, 0xC0, 0xB0, 0xA0, 0xEF, 0xBE, 0x03};
    CHECK(bytes == expected);

    mark4::FrameHeader decoded;
    REQUIRE(mark4::decodeFrameHeader(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.src == header.src);
    CHECK(decoded.dst == header.dst);
    CHECK(decoded.seq == header.seq);
    CHECK(decoded.hops == header.hops);
    CHECK(!mark4::decodeFrameHeader(bytes.data(), bytes.size() - 1U, decoded));
}

TEST_CASE("a node id hash is never the broadcast id")
{
    const std::array<std::uint8_t, 12> uid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(mark4::hashNodeId(uid.data(), uid.size()) != 0U);
    CHECK(mark4::hashNodeId(uid.data(), uid.size()) == mark4::hashNodeId(uid.data(), uid.size()));
    CHECK(mark4::hashNodeId(uid.data(), uid.size()) != mark4::hashNodeId(uid.data(), 4U));
    CHECK(mark4::randomNodeId() != 0U);
}

TEST_CASE("a transport needs a node id and a link")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport nothing(0U);
    CHECK(!nothing.init());
    mark4::Transport lonely(NODE_A);
    CHECK(!lonely.init());
    REQUIRE(lonely.addLink(link));
    CHECK(lonely.init());
}

TEST_CASE("nodes are learnt from any frame, delivered to, and expire with callbacks")
{
    FakeBus bus;
    FakeLink linkA(bus);
    FakeLink linkB(bus);
    mark4::Transport a(NODE_A);
    mark4::Transport b(NODE_B);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    Observer seenByA;
    Observer seenByB;
    seenByA.attach(a);
    seenByB.attach(b);

    // A knows nobody: a unicast to B cannot leave.
    CHECK(!a.send(NODE_B, HELLO.data(), HELLO.size()));
    CHECK(a.dropped() == 1U);

    // B broadcasts a plain payload, no beacon: A learns B from it.
    REQUIRE(b.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByA.poll(a, T0_US);
    REQUIRE(seenByA.up == std::vector<std::uint32_t>{NODE_B});
    REQUIRE(seenByA.delivered.size() == 1U);
    CHECK(seenByA.delivered[0].first == NODE_B);
    CHECK(seenByA.delivered[0].second == HELLO);
    CHECK(a.isAlive(NODE_B));
    REQUIRE(a.findNode(NODE_B) != nullptr);
    CHECK(a.findNode(NODE_B)->address.host == 1U);

    // B's own echo of its broadcast is not a node and not a delivery.
    seenByB.poll(b, T0_US);
    CHECK(seenByB.up.empty());
    CHECK(seenByB.delivered.empty());

    // Now A can answer B by id, and B learns A from that answer.
    REQUIRE(a.send(NODE_B, HELLO.data(), HELLO.size()));
    CHECK(linkA.sent() == 1U);
    seenByB.poll(b, T0_US);
    REQUIRE(seenByB.up == std::vector<std::uint32_t>{NODE_A});
    REQUIRE(seenByB.delivered.size() == 1U);
    CHECK(seenByB.delivered[0].first == NODE_A);

    // A frame for somebody else is neither delivered nor relayed by default.
    inject(bus, 1U, 0U, rawFrame(NODE_A, NODE_C, 7U, HELLO));
    seenByB.poll(b, T0_US);
    CHECK(seenByB.delivered.size() == 1U);
    CHECK(linkB.sent() == 0U);

    // Silence: B expires from A's table exactly once.
    seenByA.poll(a, T0_US + mark4::Transport::NODE_EXPIRY_US - 1U);
    CHECK(seenByA.down.empty());
    seenByA.poll(a, T0_US + mark4::Transport::NODE_EXPIRY_US);
    CHECK(seenByA.down == std::vector<std::uint32_t>{NODE_B});
    CHECK(!a.isAlive(NODE_B));
    CHECK(a.nodeCount() == 0U);
    seenByA.poll(a, T0_US + 2U * mark4::Transport::NODE_EXPIRY_US);
    CHECK(seenByA.down.size() == 1U);
}

TEST_CASE("the beacon goes out once per period and at once to a newcomer")
{
    FakeBus bus;
    FakeLink linkA(bus);
    FakeLink linkB(bus);
    mark4::Transport a(NODE_A);
    mark4::Transport b(NODE_B);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    a.setBeacon(BEACON_A.data(), BEACON_A.size());
    Observer seenByA;
    Observer seenByB;

    // First poll: the beacon goes out immediately.
    seenByA.poll(a, T0_US);
    CHECK(linkA.broadcasts() == 1U);
    seenByA.poll(a, T0_US + mark4::Transport::BEACON_PERIOD_US - 1U);
    CHECK(linkA.broadcasts() == 1U);
    seenByA.poll(a, T0_US + mark4::Transport::BEACON_PERIOD_US);
    CHECK(linkA.broadcasts() == 2U);

    // B hears the beacons as ordinary broadcast payloads.
    seenByB.poll(b, T0_US);
    REQUIRE(seenByB.delivered.size() == 2U);
    CHECK(seenByB.delivered[0].second == BEACON_A);
    CHECK(b.isAlive(NODE_A));

    // C appears through a plain frame: A unicasts its beacon to C at once,
    // without waiting for the period.
    FakeLink linkC(bus);
    mark4::Transport c(NODE_C);
    REQUIRE(c.addLink(linkC));
    Observer seenByC;
    REQUIRE(c.send(NODE_A, HELLO.data(), HELLO.size()) == false); // unknown yet
    REQUIRE(c.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    const std::uint64_t midPeriod = T0_US + mark4::Transport::BEACON_PERIOD_US + 1000U;
    seenByA.poll(a, midPeriod);
    CHECK(linkA.sent() == 1U);
    CHECK(linkA.broadcasts() == 2U);
    seenByC.poll(c, midPeriod);
    REQUIRE(seenByC.delivered.size() == 1U);
    CHECK(seenByC.delivered[0].first == NODE_A);
    CHECK(seenByC.delivered[0].second == BEACON_A);
    CHECK(c.isAlive(NODE_A));
}

TEST_CASE("a send names the links it may leave on, the beacon takes them all")
{
    // The relay's shape: a slow link the node's own chatter must stay off,
    // and the LAN it belongs on.
    FakeBus uartBus;
    FakeBus lanBus;
    FakeLink uart(uartBus);
    FakeLink board(uartBus); // the node behind the slow link, endpoint 1 of its bus
    FakeLink lan(lanBus);
    constexpr std::uint32_t LAN_ONLY = 1U << 1U;
    mark4::Transport relay(NODE_A);
    REQUIRE(relay.addLink(uart));
    REQUIRE(relay.addLink(lan));
    Observer seen;

    REQUIRE(relay.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size(), LAN_ONLY));
    CHECK(uart.broadcasts() == 0U);
    CHECK(lan.broadcasts() == 1U);

    // The beacon names no mask: it goes out on both links, so the node
    // behind the slow one learns this node too.
    relay.setBeacon(BEACON_A.data(), BEACON_A.size());
    seen.poll(relay, T0_US);
    CHECK(uart.broadcasts() == 1U);
    CHECK(lan.broadcasts() == 2U);

    // A unicast to a node sitting on an excluded link does not go out.
    inject(uartBus, 0U, 1U, rawFrame(NODE_B, mark4::BROADCAST_NODE, 1U, HELLO));
    seen.poll(relay, T0_US + 1000U);
    REQUIRE(relay.isAlive(NODE_B));
    const std::uint32_t sentBefore = uart.sent();
    CHECK(relay.send(NODE_B, HELLO.data(), HELLO.size(), LAN_ONLY) == false);
    CHECK(uart.sent() == sentBefore);
    CHECK(relay.send(NODE_B, HELLO.data(), HELLO.size()));
    CHECK(uart.sent() == sentBefore + 1U);
}

TEST_CASE("the send-side counters follow what actually left on a link")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport transport(NODE_A);
    REQUIRE(transport.addLink(link));

    const std::array<std::uint8_t, 4> payload{1U, 2U, 3U, 4U};

    // A broadcast always reaches the medium.
    REQUIRE(transport.send(mark4::BROADCAST_NODE, payload.data(), payload.size()));
    REQUIRE(transport.sent() == 1U);
    REQUIRE(transport.sentBytes() == payload.size());
    REQUIRE(transport.refused() == 0U);

    // A unicast to a node never heard of reaches nothing.
    REQUIRE(!transport.send(NODE_B, payload.data(), payload.size()));
    REQUIRE(transport.sent() == 1U);
    REQUIRE(transport.sentBytes() == payload.size());
    REQUIRE(transport.refused() == 1U);

    // A payload longer than a frame can carry never even reaches the codec.
    const std::vector<std::uint8_t> oversized(mark4::MAX_PAYLOAD + 1U, 0xEEU);
    REQUIRE(!transport.send(mark4::BROADCAST_NODE, oversized.data(), oversized.size()));
    REQUIRE(transport.refused() == 2U);

    // The beacon is one more send of this node's own and counts as one.
    transport.setBeacon(payload.data(), payload.size());
    Observer observer;
    transport.poll(T0_US, &Observer::Deliver, &observer);
    REQUIRE(transport.sent() == 2U);
    REQUIRE(transport.sentBytes() == 2U * payload.size());
}

TEST_CASE("sequence accounting counts losses and duplicates across the wrap")
{
    FakeBus bus;
    FakeLink linkA(bus);
    mark4::Transport a(NODE_A);
    REQUIRE(a.addLink(linkA));
    Observer seen;

    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0xFFFDU, HELLO));
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0xFFFEU, HELLO));
    // 0xFFFF lost, then the wrap to 0.
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0x0000U, HELLO));
    // A duplicate of 0.
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0x0000U, HELLO));
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0x0001U, HELLO));
    seen.poll(a, T0_US);

    REQUIRE(a.findNode(NODE_B) != nullptr);
    const mark4::Transport::Node &node = *a.findNode(NODE_B);
    CHECK(node.received == 4U);
    CHECK(node.lost == 1U);
    CHECK(node.duplicates == 1U);
    CHECK(node.lastSeq == 1U);
    // The duplicate was not delivered twice.
    CHECK(seen.delivered.size() == 4U);

    // A jump too large to be a loss is a restarted sender: no loss counted.
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 0x5000U, HELLO));
    seen.poll(a, T0_US);
    CHECK(node.lost == 1U);
    CHECK(node.received == 5U);
}

TEST_CASE("a relay in a line forwards towards the destination and no further")
{
    // A --bus1-- R --bus2-- C, R relays.
    FakeBus bus1;
    FakeBus bus2;
    FakeLink linkA(bus1);
    FakeLink linkR1(bus1);
    FakeLink linkR2(bus2);
    FakeLink linkC(bus2);
    mark4::Transport a(NODE_A);
    mark4::Transport r(0xBEEF0000U);
    mark4::Transport c(NODE_C);
    REQUIRE(a.addLink(linkA));
    REQUIRE(r.addLink(linkR1));
    REQUIRE(r.addLink(linkR2));
    REQUIRE(c.addLink(linkC));
    r.setRelay(true);
    Observer seenByA;
    Observer seenByR;
    Observer seenByC;

    // Both ends broadcast: R learns them on each side and floods across.
    REQUIRE(a.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    REQUIRE(c.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == 2U);
    CHECK(linkR2.broadcasts() == 1U);
    CHECK(linkR1.broadcasts() == 1U);
    seenByA.poll(a, T0_US);
    seenByC.poll(c, T0_US);
    REQUIRE(seenByA.delivered.size() == 1U);
    CHECK(seenByA.delivered[0].first == NODE_C);
    REQUIRE(seenByC.delivered.size() == 1U);
    CHECK(seenByC.delivered[0].first == NODE_A);
    // The relayed frame arrived with one hop less.
    CHECK(a.isAlive(NODE_C));
    CHECK(c.isAlive(NODE_A));

    // A unicast from A to C crosses R on the link C was heard on, and only
    // that one. R does not deliver it to itself.
    REQUIRE(a.send(NODE_C, HELLO.data(), HELLO.size()));
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == 2U);
    CHECK(linkR2.sent() == 1U);
    CHECK(linkR1.sent() == 0U);
    seenByC.poll(c, T0_US);
    REQUIRE(seenByC.delivered.size() == 2U);
    CHECK(seenByC.delivered[1].first == NODE_A);

    // A frame for C arriving on C's own side is not sent back (split horizon).
    inject(bus2, 0U, 1U, rawFrame(NODE_B, NODE_C, 1U, HELLO));
    const std::uint32_t droppedBefore = r.dropped();
    seenByR.poll(r, T0_US);
    CHECK(linkR2.sent() == 1U);
    CHECK(r.dropped() == droppedBefore + 1U);

    // Hops exhaustion: a frame with 1 hop left is delivered locally when
    // for R, and dropped rather than relayed otherwise.
    std::vector<std::uint8_t> tired = rawFrame(NODE_B, mark4::BROADCAST_NODE, 2U, HELLO);
    tired[mark4::FRAME_HEADER_SIZE - 1U] = 1U;
    inject(bus1, 1U, 0U, tired);
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == 3U);
    CHECK(linkR2.broadcasts() == 1U);
}

TEST_CASE("a relay forwards a board broadcast to the lan exactly once and ignores its echo")
{
    // board --bus1 (the UART)-- hub --bus2 (the LAN)-- sim. The hub relays
    // here (no filter): the LAN echoes every broadcast back to its sender,
    // the UART does not.
    FakeBus uart;
    FakeBus lan;
    FakeLink linkBoard(uart);
    FakeLink linkHubUart(uart);
    FakeLink linkHubLan(lan);
    FakeLink linkSim(lan);
    mark4::Transport board(NODE_A);
    mark4::Transport hub(0x4B000000U);
    mark4::Transport sim(NODE_C);
    REQUIRE(board.addLink(linkBoard));
    REQUIRE(hub.addLink(linkHubUart));
    REQUIRE(hub.addLink(linkHubLan));
    REQUIRE(sim.addLink(linkSim));
    hub.setRelay(true);
    Observer seenByBoard;
    Observer seenByHub;
    Observer seenBySim;

    // Board status: delivered to the hub once, on the LAN once, to the sim
    // once. The hub then hears its own forwarding come back on the LAN.
    REQUIRE(board.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    REQUIRE(seenByHub.delivered.size() == 1U);
    CHECK(linkHubLan.broadcasts() == 1U);
    CHECK(linkHubUart.broadcasts() == 0U);
    seenBySim.poll(sim, T0_US);
    REQUIRE(seenBySim.delivered.size() == 1U);
    CHECK(seenBySim.delivered[0].first == NODE_A);
    seenByHub.poll(hub, T0_US);
    CHECK(seenByHub.delivered.size() == 1U);
    CHECK(linkHubLan.broadcasts() == 1U);
    // The echo of its own forwarding is dropped as a duplicate of the board's
    // frame, and counted as one: the transport cannot tell it from a loop.
    // (A UdpLink filters its own echoes before they get here; this fake
    // medium does not, so the count shows.)
    REQUIRE(hub.findNode(NODE_A) != nullptr);
    CHECK(hub.findNode(NODE_A)->duplicates == 1U);
    CHECK(hub.findNode(NODE_A)->received == 1U);

    // The other way: a LAN broadcast reaches the board through the UART.
    REQUIRE(sim.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(seenByHub.delivered.size() == 2U);
    CHECK(linkHubUart.broadcasts() == 1U);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == 1U);
    CHECK(seenByBoard.delivered[0].first == NODE_C);

    // A command from the sim to the board crosses to the UART; the board's
    // unicast answer crosses back.
    REQUIRE(sim.send(NODE_A, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(linkHubUart.sent() == 1U);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == 2U);
    REQUIRE(board.send(NODE_C, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(linkHubLan.sent() == 1U);
    seenBySim.poll(sim, T0_US);
    REQUIRE(seenBySim.delivered.size() == 2U);
    CHECK(seenBySim.delivered[1].first == NODE_A);
}

namespace
{
    /// @brief Encodes one Envelope holding the named body, zeroed.
    std::vector<std::uint8_t> envelopeBytes(pb_size_t whichBody)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = whichBody;
        std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE);
        std::size_t size = 0U;
        REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
        bytes.resize(size);
        return bytes;
    }

    /// Index of the UART link on the relay under test.
    constexpr std::size_t UART_LINK = 0U;

    /// The ESP32's rule: a broadcast only reaches the UART when it is an
    /// Announce; unicasts routed there are for the board by construction.
    bool uartFilter(void *context,
                    std::size_t linkIndex,
                    const mark4::FrameHeader &header,
                    const std::uint8_t *payload,
                    std::size_t size)
    {
        static_cast<void>(context);
        return linkIndex != UART_LINK || header.dst != mark4::BROADCAST_NODE ||
               mark4::envelopeIsAnnounce(payload, size);
    }
} // namespace

TEST_CASE("the announce check reads one byte of the envelope")
{
    CHECK(mark4::envelopeIsAnnounce(envelopeBytes(mark4_Envelope_announce_tag).data(),
                                    envelopeBytes(mark4_Envelope_announce_tag).size()));
    for (const pb_size_t other : {mark4_Envelope_status_tag,
                                  mark4_Envelope_rc_tag,
                                  mark4_Envelope_log_tag,
                                  mark4_Envelope_ota_chunk_tag,
                                  mark4_Envelope_sim_run_stats_tag})
    {
        const std::vector<std::uint8_t> bytes = envelopeBytes(other);
        CHECK(!mark4::envelopeIsAnnounce(bytes.data(), bytes.size()));
    }
    CHECK(!mark4::envelopeIsAnnounce(nullptr, 1U));
    CHECK(!mark4::envelopeIsAnnounce(HELLO.data(), 0U));
}

TEST_CASE(
    "a relay filter keeps lan broadcasts off the uart and lets commands and announces through")
{
    // board --uart-- relay (ESP32: relays, no beacon, filter) --lan-- hub, sim.
    FakeBus uart;
    FakeBus lan;
    FakeLink linkBoard(uart);
    FakeLink linkRelayUart(uart);
    FakeLink linkRelayLan(lan);
    FakeLink linkHub(lan);
    FakeLink linkSim(lan);
    mark4::Transport board(NODE_A);
    mark4::Transport relay(0xE5320000U);
    mark4::Transport hub(NODE_B);
    mark4::Transport sim(NODE_C);
    REQUIRE(board.addLink(linkBoard));
    REQUIRE(relay.addLink(linkRelayUart)); // UART_LINK
    REQUIRE(relay.addLink(linkRelayLan));
    REQUIRE(hub.addLink(linkHub));
    REQUIRE(sim.addLink(linkSim));
    relay.setRelay(true);
    relay.setRelayFilter(&uartFilter, nullptr);
    Observer seenByBoard;
    Observer seenByRelay;
    Observer seenByHub;
    Observer seenBySim;

    const std::vector<std::uint8_t> announce = envelopeBytes(mark4_Envelope_announce_tag);
    const std::vector<std::uint8_t> status = envelopeBytes(mark4_Envelope_status_tag);
    const std::vector<std::uint8_t> command = envelopeBytes(mark4_Envelope_rc_tag);
    board.setBeacon(announce.data(), announce.size());
    hub.setBeacon(announce.data(), announce.size());
    sim.setBeacon(announce.data(), announce.size());

    // Beacons: every node's Announce crosses the relay in both directions.
    seenByBoard.poll(board, T0_US);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayLan.broadcasts() == 1U);  // the board's announce, once
    CHECK(linkRelayUart.broadcasts() == 2U); // the hub's and the sim's
    CHECK(relay.relayed() == 3U);
    CHECK(relay.filtered() == 0U);
    // Everyone learns the board from the relayed frame, and the board
    // learns everyone; the relay itself, silent, is known to nobody.
    seenByBoard.poll(board, T0_US);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    CHECK(hub.isAlive(NODE_A));
    CHECK(sim.isAlive(NODE_A));
    CHECK(board.isAlive(NODE_B));
    CHECK(board.isAlive(NODE_C));
    CHECK(!hub.isAlive(relay.nodeId()));
    CHECK(hub.nodeCount() == 2U);
    // The learners unicast their beacon to the newcomer: the hub's and the
    // sim's are frames for the board and cross the UART; the board's two
    // cross the other way and land on the LAN nodes exactly once each.
    seenByRelay.poll(relay, T0_US);
    seenByBoard.poll(board, T0_US);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    CHECK(linkRelayUart.sent() == 2U);
    CHECK(linkRelayLan.sent() == 2U);
    CHECK(seenByBoard.delivered.size() == 4U);
    CHECK(seenByHub.delivered.back().first == NODE_A);
    CHECK(seenBySim.delivered.back().first == NODE_A);
    CHECK(relay.filtered() == 0U);
    // The echo of its own forwarding of the board's announce is a duplicate
    // for the relay (this fake LAN echoes; a UdpLink drops its own echoes).
    REQUIRE(relay.findNode(NODE_A) != nullptr);
    const std::uint32_t duplicatesBefore = relay.findNode(NODE_A)->duplicates;

    // The sim's status broadcast reaches the hub and stops at the relay:
    // the board's UART never sees it.
    const std::uint32_t uartBefore = linkRelayUart.broadcasts();
    const std::size_t boardBefore = seenByBoard.delivered.size();
    REQUIRE(sim.send(mark4::BROADCAST_NODE, status.data(), status.size()));
    seenByRelay.poll(relay, T0_US);
    seenByHub.poll(hub, T0_US);
    seenByBoard.poll(board, T0_US);
    CHECK(seenByHub.delivered.back().second == status);
    CHECK(linkRelayUart.broadcasts() == uartBefore);
    CHECK(seenByBoard.delivered.size() == boardBefore);
    CHECK(relay.filtered() == 1U);
    CHECK(relay.dropped() == 0U);

    // A command from the hub to the board is a unicast: it crosses.
    REQUIRE(hub.send(NODE_A, command.data(), command.size()));
    seenByRelay.poll(relay, T0_US);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == boardBefore + 1U);
    CHECK(seenByBoard.delivered.back().first == NODE_B);
    CHECK(seenByBoard.delivered.back().second == command);
    CHECK(relay.filtered() == 1U);

    // The board's status broadcast reaches the LAN exactly once, with
    // one hop less, and both LAN nodes get it once.
    const std::uint32_t lanBefore = linkRelayLan.broadcasts();
    const std::size_t hubBefore = seenByHub.delivered.size();
    const std::size_t simBefore = seenBySim.delivered.size();
    REQUIRE(board.send(mark4::BROADCAST_NODE, status.data(), status.size()));
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayLan.broadcasts() == lanBefore + 1U);
    // The copy waiting in the hub's inbox (LAN endpoint 1) has one hop less.
    mark4::FrameHeader forwarded;
    REQUIRE(!lan.inbox[1].empty());
    REQUIRE(mark4::decodeFrameHeader(
        lan.inbox[1].back().bytes.data(), lan.inbox[1].back().bytes.size(), forwarded));
    CHECK(forwarded.hops == mark4::Transport::INITIAL_HOPS - 1U);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    CHECK(seenByHub.delivered.size() == hubBefore + 1U);
    CHECK(seenBySim.delivered.size() == simBefore + 1U);
    CHECK(seenByHub.delivered.back().second == status);
    // The echo of its own forwarding is one more duplicate for the relay,
    // and it is not forwarded back onto the UART.
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayUart.broadcasts() == uartBefore);
    CHECK(relay.findNode(NODE_A)->duplicates == duplicatesBefore + 1U);
}

TEST_CASE("relays in a triangle never loop a broadcast")
{
    // Three relays, each on two buses, every pair sharing one bus.
    FakeBus busAB;
    FakeBus busBC;
    FakeBus busCA;
    FakeLink aOnAB(busAB);
    FakeLink bOnAB(busAB);
    FakeLink bOnBC(busBC);
    FakeLink cOnBC(busBC);
    FakeLink cOnCA(busCA);
    FakeLink aOnCA(busCA);
    mark4::Transport a(NODE_A);
    mark4::Transport b(NODE_B);
    mark4::Transport c(NODE_C);
    REQUIRE(a.addLink(aOnAB));
    REQUIRE(a.addLink(aOnCA));
    REQUIRE(b.addLink(bOnAB));
    REQUIRE(b.addLink(bOnBC));
    REQUIRE(c.addLink(cOnBC));
    REQUIRE(c.addLink(cOnCA));
    for (mark4::Transport *node : {&a, &b, &c})
    {
        node->setRelay(true);
    }
    Observer seenByA;
    Observer seenByB;
    Observer seenByC;

    REQUIRE(a.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    // Round after round until every inbox is empty: a loop would never end.
    for (unsigned round = 0U; round < 16U; ++round)
    {
        seenByA.poll(a, T0_US);
        seenByB.poll(b, T0_US);
        seenByC.poll(c, T0_US);
    }
    bool quiet = true;
    for (FakeBus *bus : {&busAB, &busBC, &busCA})
    {
        for (const auto &queue : bus->inbox)
        {
            quiet = quiet && queue.empty();
        }
    }
    CHECK(quiet);
    // Each peer got the payload exactly once; every further copy (the other
    // relay's forward, the echo of its own) was a duplicate.
    CHECK(seenByA.delivered.empty());
    CHECK(seenByB.delivered.size() == 1U);
    CHECK(seenByC.delivered.size() == 1U);
    REQUIRE(b.findNode(NODE_A) != nullptr);
    CHECK(b.findNode(NODE_A)->duplicates >= 1U);
    REQUIRE(c.findNode(NODE_A) != nullptr);
    CHECK(c.findNode(NODE_A)->duplicates >= 1U);
    // Broadcasts: A's own on two links, one forward per relay.
    CHECK(aOnAB.broadcasts() + aOnCA.broadcasts() == 2U);
    CHECK(bOnBC.broadcasts() == 1U);
    CHECK(cOnCA.broadcasts() == 1U);
}

TEST_CASE("the uart link frames payloads and resynchronizes after garbage and a torn frame")
{
    mark4::BytePipe pipe;
    mark4::PipeEnd endA(pipe.toA, pipe.toB);
    mark4::PipeEnd endB(pipe.toB, pipe.toA);
    mark4::UartLink linkA(endA);
    mark4::UartLink linkB(endB);

    const std::vector<std::uint8_t> first = rawFrame(NODE_A, NODE_B, 1U, HELLO);
    const std::vector<std::uint8_t> second = rawFrame(NODE_A, NODE_B, 2U, HELLO);
    const std::vector<std::uint8_t> third = rawFrame(NODE_A, NODE_B, 3U, HELLO);
    const std::vector<std::uint8_t> fourth = rawFrame(NODE_A, NODE_B, 4U, HELLO);

    // Garbage, a whole frame, a frame torn after three payload bytes, then
    // two whole frames. The torn frame swallows the next one up to its
    // announced length and fails its CRC; the parser then hunts for the
    // sync pair again and the last frame comes out whole.
    const std::array<std::uint8_t, 4> garbage = {0x00, 0xA5, 0x12, 0xFF};
    REQUIRE(endA.write(garbage.data(), garbage.size()));
    REQUIRE(linkA.broadcast(first.data(), first.size()));
    std::array<std::uint8_t, 64> torn{};
    const std::size_t tornSize =
        mark4::encodeSerialFrame(second.data(), second.size(), torn.data());
    REQUIRE(tornSize == second.size() + mark4::SERIAL_FRAME_OVERHEAD);
    REQUIRE(endA.write(torn.data(), 6U));
    REQUIRE(linkA.send(third.data(), third.size(), mark4::LinkAddress{}));
    REQUIRE(linkA.send(fourth.data(), fourth.size(), mark4::LinkAddress{}));

    std::array<std::uint8_t, mark4::MAX_FRAME_SIZE> out{};
    mark4::LinkAddress from;
    std::size_t size = linkB.receive(out.data(), out.size(), from);
    REQUIRE(size == first.size());
    CHECK(std::memcmp(out.data(), first.data(), size) == 0);
    CHECK(from.host == 0U);
    size = linkB.receive(out.data(), out.size(), from);
    REQUIRE(size == fourth.size());
    CHECK(std::memcmp(out.data(), fourth.data(), size) == 0);
    CHECK(linkB.receive(out.data(), out.size(), from) == 0U);

    // Two frames landing in one read chunk both come out.
    REQUIRE(linkA.broadcast(first.data(), first.size()));
    REQUIRE(linkA.broadcast(second.data(), second.size()));
    REQUIRE(linkB.receive(out.data(), out.size(), from) == first.size());
    REQUIRE(linkB.receive(out.data(), out.size(), from) == second.size());
    CHECK(linkB.receive(out.data(), out.size(), from) == 0U);

    // A frame the serial length byte cannot describe is refused.
    const std::vector<std::uint8_t> huge(mark4::SERIAL_MAX_PAYLOAD + 1U, 0x55);
    CHECK(!linkA.broadcast(huge.data(), huge.size()));

    // Two transports over the pipe talk like over any other link.
    mark4::Transport a(NODE_A);
    mark4::Transport b(NODE_B);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    Observer seenByB;
    REQUIRE(a.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByB.poll(b, T0_US);
    REQUIRE(seenByB.delivered.size() == 1U);
    CHECK(seenByB.delivered[0].first == NODE_A);
    CHECK(seenByB.delivered[0].second == HELLO);
    REQUIRE(b.send(NODE_A, HELLO.data(), HELLO.size()));
    Observer seenByA;
    seenByA.poll(a, T0_US);
    REQUIRE(seenByA.delivered.size() == 1U);
    CHECK(seenByA.delivered[0].first == NODE_B);
}

namespace
{
    /// How long a loopback datagram may take to show up [us], polled.
    constexpr unsigned UDP_WAIT_STEPS = 200U;
    constexpr unsigned UDP_WAIT_STEP_US = 1000U;

    /// @brief Polls a transport until a predicate holds or the wait expires.
    template <typename Predicate>
    bool pollUntil(mark4::Transport &transport, Observer &observer, Predicate predicate)
    {
        for (unsigned step = 0U; step < UDP_WAIT_STEPS; ++step)
        {
            observer.poll(transport, T0_US + static_cast<std::uint64_t>(step) * UDP_WAIT_STEP_US);
            if (predicate())
            {
                return true;
            }
            ::usleep(UDP_WAIT_STEP_US);
        }
        return false;
    }

    /// @return a UDP port nothing holds right now
    std::uint16_t pickFreePort()
    {
        mark4::UdpLink probe(0U);
        REQUIRE(probe.init());
        return probe.dataPort();
    }
} // namespace

TEST_CASE("two udp nodes on one host find each other through the shared discovery port", "[udp]")
{
    const std::uint16_t discoveryPort = pickFreePort();
    mark4::UdpLink linkA(discoveryPort);
    mark4::UdpLink linkB(discoveryPort);
    REQUIRE(linkA.init());
    REQUIRE(linkB.init());
    CHECK(linkA.dataPort() != linkB.dataPort());
    CHECK(linkA.discoveryFd() >= 0);
    CHECK(linkA.dataFd() >= 0);

    mark4::Transport a(NODE_A);
    mark4::Transport b(NODE_B);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    REQUIRE(a.init());
    REQUIRE(b.init());
    a.setBeacon(BEACON_A.data(), BEACON_A.size());
    b.setBeacon(BEACON_B.data(), BEACON_B.size());
    Observer seenByA;
    Observer seenByB;

    // Both beacons are broadcast on the first poll; each side learns the
    // other. A sandbox that forbids every broadcast route fails here, and
    // that is the signal to read: nothing else in this test can work.
    seenByA.poll(a, T0_US);
    seenByB.poll(b, T0_US);
    const bool heard = pollUntil(a, seenByA, [&a] { return a.isAlive(NODE_B); }) &&
                       pollUntil(b, seenByB, [&b] { return b.isAlive(NODE_A); });
    if (!heard)
    {
        WARN("udp broadcast did not reach the discovery sockets: is broadcast forbidden here?");
    }
    REQUIRE(heard);
    REQUIRE(a.findNode(NODE_B) != nullptr);
    CHECK(a.findNode(NODE_B)->address.port == linkB.dataPort());
    REQUIRE(!seenByA.delivered.empty());
    CHECK(seenByA.delivered[0].second == BEACON_B);

    // Unicast by id goes to the data socket of the peer.
    REQUIRE(a.send(NODE_B, HELLO.data(), HELLO.size()));
    REQUIRE(pollUntil(b, seenByB, [&seenByB] {
        return !seenByB.delivered.empty() && seenByB.delivered.back().second == HELLO;
    }));
    CHECK(seenByB.delivered.back().first == NODE_A);

    // A node on another discovery port is on another deployment: unheard.
    mark4::UdpLink linkC(pickFreePort());
    REQUIRE(linkC.init());
    mark4::Transport c(NODE_C);
    REQUIRE(c.addLink(linkC));
    Observer seenByC;
    c.setBeacon(BEACON_A.data(), BEACON_A.size());
    seenByC.poll(c, T0_US);
    CHECK(!pollUntil(a, seenByA, [&a] { return a.isAlive(NODE_C); }));
}
