/// @file
/// @brief Transport core over fake links: header codec, node table, presence
///        listeners, keepalive, sequence accounting, relay; the UART link over an
///        in-memory byte pipe; the UDP link between two nodes on one host.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <deque>
#include <variant>
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
    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D00U;
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
            const auto *udp = std::get_if<mark4::UdpAddress>(&address);
            if (udp == nullptr || udp->host >= m_bus.inbox.size())
            {
                return false;
            }
            m_bus.inbox[udp->host].push_back(
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
            return mark4::UdpAddress{m_index, 1U};
        }

        FakeBus &m_bus;                  ///< the medium
        std::uint32_t m_index;           ///< this endpoint's address on it
        std::uint32_t m_sent = 0U;       ///< unicast frames sent
        std::uint32_t m_broadcasts = 0U; ///< broadcast frames sent
    };

    /// Everything an application would observe from one transport: the
    /// payloads delivered to it and, as a presence listener, the nodes that
    /// came and went.
    struct Observer final : mark4::AbsPresenceListener
    {
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>
            delivered;                   ///< (src, payload)
        std::vector<std::uint32_t> up;   ///< nodes that appeared, in order
        std::vector<std::uint32_t> down; ///< nodes that expired, in order

        explicit Observer(mark4::Transport &transport)
            : AbsPresenceListener(transport)
        {
        }

        static void Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size)
        {
            static_cast<Observer *>(context)->delivered.emplace_back(
                src, std::vector<std::uint8_t>(payload, payload + size));
        }

        void onNodeUp(const mark4::Transport::Node &node) override
        {
            up.push_back(node.id);
        }

        void onNodeDown(const mark4::Transport::Node &node) override
        {
            down.push_back(node.id);
        }

        void poll(mark4::Transport &transport, std::uint64_t nowUs)
        {
            transport.poll(nowUs, &Observer::Deliver, this);
        }
    };

    const std::vector<std::uint8_t> HELLO = {'h', 'e', 'l', 'l', 'o'};

    /// No payload: what a frame that is nobody's message carries.
    const std::vector<std::uint8_t> NOTHING;

    /// Boot id a foreign keepalive announces, and the other incarnation of
    /// the same node.
    constexpr std::uint32_t PEER_BOOT = 0x11223344U;
    constexpr std::uint32_t PEER_REBOOT = 0x55667788U;

    /// @brief Builds one raw frame the way a foreign sender would: hops = 0,
    ///        no relay crossed yet.
    std::vector<std::uint8_t> rawFrame(std::uint32_t src,
                                       std::uint32_t dst,
                                       std::uint16_t seq,
                                       const std::vector<std::uint8_t> &payload)
    {
        mark4::FrameHeader header;
        header.src = src;
        header.dst = dst;
        header.seq = seq;
        header.hops = 0U;
        std::vector<std::uint8_t> frame(mark4::FRAME_HEADER_SIZE + payload.size());
        mark4::encodeFrameHeader(header, frame.data());
        if (!payload.empty())
        {
            std::memcpy(frame.data() + mark4::FRAME_HEADER_SIZE, payload.data(), payload.size());
        }
        return frame;
    }

    /// @brief Builds one raw keepalive the way a foreign node's transport
    ///        would: the header flagged, the boot id behind it.
    std::vector<std::uint8_t> keepaliveFrame(std::uint32_t src,
                                             std::uint32_t dst,
                                             std::uint16_t seq,
                                             std::uint32_t boot)
    {
        std::vector<std::uint8_t> payload(mark4::KEEPALIVE_PAYLOAD_SIZE, 0U);
        for (std::size_t index = 0U; index < payload.size(); ++index)
        {
            payload[index] = static_cast<std::uint8_t>(boot >> (8U * static_cast<unsigned>(index)));
        }
        std::vector<std::uint8_t> frame = rawFrame(src, dst, seq, payload);
        frame[mark4::FRAME_HEADER_SIZE - 1U] = static_cast<std::uint8_t>(
            frame[mark4::FRAME_HEADER_SIZE - 1U] | mark4::FRAME_FLAG_KEEPALIVE);
        return frame;
    }

    /// @brief Pushes one raw frame into a bus endpoint's inbox.
    void inject(FakeBus &bus,
                std::size_t endpoint,
                std::uint32_t fromEndpoint,
                const std::vector<std::uint8_t> &frame)
    {
        bus.inbox[endpoint].push_back(Datagram{frame, mark4::UdpAddress{fromEndpoint, 1U}, false});
    }

    /// One node of a settle(): its transport and the observer polling it.
    using Peer = std::pair<mark4::Transport *, Observer *>;

    /// @brief Polls every peer in turn, all at the same instant, until no
    ///        inbox of any bus holds a frame: the keepalives every newcomer
    ///        triggers have all been heard and everyone knows everyone. A
    ///        network that does not quiet down within a few rounds fails.
    void settle(const std::vector<Peer> &peers,
                const std::vector<FakeBus *> &buses,
                std::uint64_t nowUs)
    {
        for (unsigned round = 0U; round < 16U; ++round)
        {
            for (const Peer &peer : peers)
            {
                peer.second->poll(*peer.first, nowUs);
            }
            bool quiet = true;
            for (const FakeBus *bus : buses)
            {
                for (const auto &queue : bus->inbox)
                {
                    quiet = quiet && queue.empty();
                }
            }
            if (quiet)
            {
                return;
            }
        }
        FAIL("the network did not settle");
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
    CHECK(!decoded.keepalive);
    CHECK(!mark4::decodeFrameHeader(bytes.data(), bytes.size() - 1U, decoded));

    // The last byte holds the hop count and the flags: the keepalive flag
    // rides on bit 7 and leaves the count alone.
    header.keepalive = true;
    mark4::encodeFrameHeader(header, bytes.data());
    CHECK(bytes[mark4::FRAME_HEADER_SIZE - 1U] == (0x03U | mark4::FRAME_FLAG_KEEPALIVE));
    REQUIRE(mark4::decodeFrameHeader(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.hops == header.hops);
    CHECK(decoded.keepalive);
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
    mark4::Transport nothing(0U, BOOT_ID);
    CHECK(!nothing.init());
    mark4::Transport lonely(NODE_A, BOOT_ID);
    CHECK(!lonely.init());
    REQUIRE(lonely.addLink(link));
    CHECK(lonely.init());
}

TEST_CASE("a transport takes at most MAX_LISTENERS presence listeners")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport transport(NODE_A, BOOT_ID);
    REQUIRE(transport.addLink(link));
    static_assert(mark4::Transport::MAX_LISTENERS == 4U);
    Observer first(transport);
    Observer second(transport);
    Observer third(transport);
    Observer fourth(transport);
    REQUIRE(transport.init());

    // A fifth one is not attached, and the composition is refused: a node
    // that loses events silently is worse than one that does not start.
    Observer fifth(transport);
    CHECK(!transport.init());
    inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 1U, HELLO));
    first.poll(transport, T0_US);
    CHECK(fourth.up == std::vector<std::uint32_t>{NODE_B});
    CHECK(fifth.up.empty());
}

TEST_CASE("a destroyed presence listener is no longer called and frees its slot")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport transport(NODE_A, BOOT_ID);
    REQUIRE(transport.addLink(link));
    Observer kept(transport);
    {
        Observer gone(transport);
        inject(bus, 0U, 1U, rawFrame(NODE_B, NODE_A, 1U, HELLO));
        kept.poll(transport, T0_US);
        CHECK(gone.up == std::vector<std::uint32_t>{NODE_B});
        CHECK(kept.up == std::vector<std::uint32_t>{NODE_B});
    }
    // The slot it held is free again: three more fit, and the events keep
    // reaching the one that stayed.
    Observer second(transport);
    Observer third(transport);
    Observer fourth(transport);
    REQUIRE(transport.init());
    inject(bus, 0U, 1U, rawFrame(NODE_C, NODE_A, 1U, HELLO));
    kept.poll(transport, T0_US);
    CHECK(kept.up == std::vector<std::uint32_t>{NODE_B, NODE_C});
    CHECK(fourth.up == std::vector<std::uint32_t>{NODE_C});
    kept.poll(transport, T0_US + mark4::Transport::NODE_EXPIRY_US);
    CHECK(kept.down.size() == 2U);
    CHECK(fourth.down.size() == 2U);
    CHECK(transport.nodeCount() == 0U);
}

TEST_CASE("nodes are learnt from any frame, delivered to, and expire with callbacks")
{
    FakeBus bus;
    FakeLink linkA(bus);
    FakeLink linkB(bus);
    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport b(NODE_B, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    Observer seenByA(a);
    Observer seenByB(b);

    // A knows nobody: a unicast to B cannot leave.
    CHECK(!a.send(NODE_B, HELLO.data(), HELLO.size()));
    CHECK(a.dropped() == 1U);

    // B broadcasts a plain payload: A learns B from it, and greets the
    // newcomer with a unicast keepalive.
    REQUIRE(b.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByA.poll(a, T0_US);
    REQUIRE(seenByA.up == std::vector<std::uint32_t>{NODE_B});
    REQUIRE(seenByA.delivered.size() == 1U);
    CHECK(seenByA.delivered[0].first == NODE_B);
    CHECK(seenByA.delivered[0].second == HELLO);
    CHECK(a.isAlive(NODE_B));
    REQUIRE(a.findNode(NODE_B) != nullptr);
    const auto *addressB = std::get_if<mark4::UdpAddress>(&a.findNode(NODE_B)->address);
    REQUIRE(addressB != nullptr);
    CHECK(addressB->host == 1U);
    CHECK(linkA.sent() == 1U);

    // B's own echo of its broadcast is not a node and not a delivery; A's
    // keepalives make A a node of B's without delivering anything.
    seenByB.poll(b, T0_US);
    CHECK(seenByB.up == std::vector<std::uint32_t>{NODE_A});
    CHECK(seenByB.delivered.empty());

    // A can answer B by id.
    REQUIRE(a.send(NODE_B, HELLO.data(), HELLO.size()));
    CHECK(linkA.sent() == 2U);
    seenByB.poll(b, T0_US);
    REQUIRE(seenByB.up == std::vector<std::uint32_t>{NODE_A});
    REQUIRE(seenByB.delivered.size() == 1U);
    CHECK(seenByB.delivered[0].first == NODE_A);

    // A frame for somebody else is not delivered, and a node with one link
    // has nowhere to relay it to.
    const std::uint32_t sentByB = linkB.sent();
    inject(bus, 1U, 0U, rawFrame(NODE_A, NODE_C, 7U, HELLO));
    seenByB.poll(b, T0_US);
    CHECK(seenByB.delivered.size() == 1U);
    CHECK(linkB.sent() == sentByB);

    // A drains what B sent at T0 (its keepalives), then silence: B expires
    // from A's table exactly once.
    seenByA.poll(a, T0_US);
    seenByA.poll(a, T0_US + mark4::Transport::NODE_EXPIRY_US - 1U);
    CHECK(seenByA.down.empty());
    seenByA.poll(a, T0_US + mark4::Transport::NODE_EXPIRY_US);
    CHECK(seenByA.down == std::vector<std::uint32_t>{NODE_B});
    CHECK(!a.isAlive(NODE_B));
    CHECK(a.nodeCount() == 0U);
    seenByA.poll(a, T0_US + 2U * mark4::Transport::NODE_EXPIRY_US);
    CHECK(seenByA.down.size() == 1U);
}

TEST_CASE("the keepalive goes out once per period and at once to a newcomer")
{
    FakeBus bus;
    FakeLink linkA(bus);
    FakeLink linkB(bus);
    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport b(NODE_B, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    Observer seenByA(a);
    Observer seenByB(b);

    // First poll: a flagged broadcast frame carrying the boot id leaves at
    // once.
    seenByA.poll(a, T0_US);
    CHECK(linkA.broadcasts() == 1U);
    REQUIRE(!bus.inbox[1].empty());
    CHECK(bus.inbox[1].back().broadcast);
    CHECK(bus.inbox[1].back().bytes.size() ==
          mark4::FRAME_HEADER_SIZE + mark4::KEEPALIVE_PAYLOAD_SIZE);
    mark4::FrameHeader keepalive;
    REQUIRE(mark4::decodeFrameHeader(
        bus.inbox[1].back().bytes.data(), bus.inbox[1].back().bytes.size(), keepalive));
    CHECK(keepalive.keepalive);
    // One per period, none in between.
    seenByA.poll(a, T0_US + mark4::Transport::KEEPALIVE_PERIOD_US - 1U);
    CHECK(linkA.broadcasts() == 1U);
    seenByA.poll(a, T0_US + mark4::Transport::KEEPALIVE_PERIOD_US);
    CHECK(linkA.broadcasts() == 2U);

    // B learns A from the keepalives; nothing is delivered.
    seenByB.poll(b, T0_US);
    CHECK(seenByB.up == std::vector<std::uint32_t>{NODE_A});
    CHECK(seenByB.delivered.empty());
    CHECK(b.isAlive(NODE_A));
    REQUIRE(b.findNode(NODE_A) != nullptr);
    CHECK(b.findNode(NODE_A)->received == 2U);
    CHECK(b.findNode(NODE_A)->boot == a.bootId());

    // C appears through a plain frame: A unicasts a keepalive to C at once,
    // without waiting for the period (and one to B, whose own keepalives
    // A hears in the same poll).
    FakeLink linkC(bus);
    mark4::Transport c(NODE_C, BOOT_ID);
    REQUIRE(c.addLink(linkC));
    Observer seenByC(c);
    REQUIRE(c.send(NODE_A, HELLO.data(), HELLO.size()) == false); // unknown yet
    REQUIRE(c.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    const std::uint64_t midPeriod = T0_US + mark4::Transport::KEEPALIVE_PERIOD_US + 1000U;
    seenByA.poll(a, midPeriod);
    CHECK(seenByA.up == std::vector<std::uint32_t>{NODE_B, NODE_C});
    CHECK(linkA.sent() == 2U);
    CHECK(linkA.broadcasts() == 2U);
    REQUIRE(!bus.inbox[2].empty());
    CHECK(!bus.inbox[2].back().broadcast);
    CHECK(bus.inbox[2].back().bytes.size() ==
          mark4::FRAME_HEADER_SIZE + mark4::KEEPALIVE_PAYLOAD_SIZE);
    // C learns A from that unicast, and nothing is delivered to it.
    seenByC.poll(c, midPeriod);
    CHECK(seenByC.up == std::vector<std::uint32_t>{NODE_A});
    CHECK(seenByC.delivered.empty());
    CHECK(c.isAlive(NODE_A));
}

TEST_CASE("a keepalive is learnt from and never delivered")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport a(NODE_A, BOOT_ID);
    REQUIRE(a.addLink(link));
    Observer seen(a);

    // Unicast to this node: the flagged frame, its boot id behind it.
    inject(bus, 0U, 1U, keepaliveFrame(NODE_B, NODE_A, 1U, PEER_BOOT));
    seen.poll(a, T0_US);
    CHECK(seen.up == std::vector<std::uint32_t>{NODE_B});
    CHECK(seen.delivered.empty());
    CHECK(a.isAlive(NODE_B));
    REQUIRE(a.findNode(NODE_B) != nullptr);
    CHECK(a.findNode(NODE_B)->boot == PEER_BOOT);

    // Broadcast: the same.
    inject(bus, 0U, 2U, keepaliveFrame(NODE_C, mark4::BROADCAST_NODE, 5U, PEER_BOOT));
    seen.poll(a, T0_US);
    CHECK(seen.up == std::vector<std::uint32_t>{NODE_B, NODE_C});
    CHECK(seen.delivered.empty());
    CHECK(a.isAlive(NODE_C));

    // It counts in the sequence accounting like any frame.
    inject(bus, 0U, 2U, keepaliveFrame(NODE_C, mark4::BROADCAST_NODE, 7U, PEER_BOOT));
    seen.poll(a, T0_US);
    REQUIRE(a.findNode(NODE_C) != nullptr);
    CHECK(a.findNode(NODE_C)->received == 2U);
    CHECK(a.findNode(NODE_C)->lost == 1U);
    CHECK(seen.delivered.empty());

    // A frame without the flag and without a payload is nobody's message
    // either: learnt from, delivered nowhere. That is what a node built
    // before the flag existed sends as its keepalive, boot id unknown.
    inject(bus, 0U, 3U, rawFrame(NODE_B + 1U, NODE_A, 1U, NOTHING));
    seen.poll(a, T0_US);
    CHECK(a.isAlive(NODE_B + 1U));
    REQUIRE(a.findNode(NODE_B + 1U) != nullptr);
    CHECK(a.findNode(NODE_B + 1U)->boot == 0U);
    CHECK(seen.delivered.empty());

    // Another incarnation of NODE_B: it leaves the table and comes back,
    // its counters reset, and every listener hears both.
    inject(bus, 0U, 1U, keepaliveFrame(NODE_B, NODE_A, 2U, PEER_REBOOT));
    seen.poll(a, T0_US);
    CHECK(seen.down == std::vector<std::uint32_t>{NODE_B});
    CHECK(seen.up == std::vector<std::uint32_t>{NODE_B, NODE_C, NODE_B + 1U, NODE_B});
    REQUIRE(a.findNode(NODE_B) != nullptr);
    CHECK(a.findNode(NODE_B)->boot == PEER_REBOOT);
    CHECK(a.findNode(NODE_B)->received == 1U);
    CHECK(seen.delivered.empty());
}

TEST_CASE("the send-side counters follow what actually left on a link")
{
    FakeBus bus;
    FakeLink link(bus);
    mark4::Transport transport(NODE_A, BOOT_ID);
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

    // An empty one is not a message: the header-only frame is the
    // transport's own keepalive.
    REQUIRE(!transport.send(mark4::BROADCAST_NODE, payload.data(), 0U));
    REQUIRE(transport.refused() == 3U);
    REQUIRE(transport.sent() == 1U);

    // The keepalive is one more send of this node's own: one frame, no
    // payload bytes.
    Observer observer(transport);
    transport.poll(T0_US, &Observer::Deliver, &observer);
    REQUIRE(transport.sent() == 2U);
    REQUIRE(transport.sentBytes() == payload.size());
    REQUIRE(transport.refused() == 3U);
}

TEST_CASE("sequence accounting counts losses and duplicates across the wrap")
{
    FakeBus bus;
    FakeLink linkA(bus);
    mark4::Transport a(NODE_A, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    Observer seen(a);

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
    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport r(0xBEEF0000U, BOOT_ID);
    mark4::Transport c(NODE_C, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    REQUIRE(r.addLink(linkR1));
    REQUIRE(r.addLink(linkR2));
    REQUIRE(c.addLink(linkC));
    Observer seenByA(a);
    Observer seenByR(r);
    Observer seenByC(c);

    // Both ends broadcast: R learns them on each side and floods across.
    // On each side one relayed frame and R's own keepalive.
    REQUIRE(a.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    REQUIRE(c.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == 2U);
    CHECK(linkR2.broadcasts() == 2U);
    CHECK(linkR1.broadcasts() == 2U);
    seenByA.poll(a, T0_US);
    seenByC.poll(c, T0_US);
    REQUIRE(seenByA.delivered.size() == 1U);
    CHECK(seenByA.delivered[0].first == NODE_C);
    REQUIRE(seenByC.delivered.size() == 1U);
    CHECK(seenByC.delivered[0].first == NODE_A);
    // The relayed frame arrived with one hop more: the far end sits one
    // relay away, the relay itself is a direct neighbour.
    CHECK(a.isAlive(NODE_C));
    CHECK(c.isAlive(NODE_A));
    REQUIRE(a.findNode(NODE_C) != nullptr);
    CHECK(a.findNode(NODE_C)->hops == 1U);
    REQUIRE(a.findNode(r.nodeId()) != nullptr);
    CHECK(a.findNode(r.nodeId())->hops == 0U);
    REQUIRE(c.findNode(NODE_A) != nullptr);
    CHECK(c.findNode(NODE_A)->hops == 1U);

    // B sits on C's side and says hello once; then the keepalives every
    // newcomer triggered settle, and everyone knows everyone.
    inject(bus2, 0U, 1U, rawFrame(NODE_B, mark4::BROADCAST_NODE, 0U, HELLO));
    settle({{&a, &seenByA}, {&r, &seenByR}, {&c, &seenByC}}, {&bus1, &bus2}, T0_US);
    CHECK(r.isAlive(NODE_B));
    const std::size_t deliveredToR = seenByR.delivered.size();
    const std::size_t deliveredToC = seenByC.delivered.size();
    const std::uint32_t sentR1 = linkR1.sent();
    const std::uint32_t sentR2 = linkR2.sent();

    // A unicast from A to C crosses R on the link C was heard on, and only
    // that one. R does not deliver it to itself.
    REQUIRE(a.send(NODE_C, HELLO.data(), HELLO.size()));
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == deliveredToR);
    CHECK(linkR2.sent() == sentR2 + 1U);
    CHECK(linkR1.sent() == sentR1);
    seenByC.poll(c, T0_US);
    REQUIRE(seenByC.delivered.size() == deliveredToC + 1U);
    CHECK(seenByC.delivered.back().first == NODE_A);

    // A frame for C arriving on C's own side is not sent back (split horizon).
    inject(bus2, 0U, 1U, rawFrame(NODE_B, NODE_C, 1U, HELLO));
    const std::uint32_t droppedBefore = r.dropped();
    seenByR.poll(r, T0_US);
    CHECK(linkR2.sent() == sentR2 + 1U);
    CHECK(r.dropped() == droppedBefore + 1U);

    // Hops exhaustion: a frame that already crossed MAX_HOPS relays is
    // delivered locally when for R, and dropped rather than relayed.
    std::vector<std::uint8_t> tired = rawFrame(NODE_B, mark4::BROADCAST_NODE, 2U, HELLO);
    tired[mark4::FRAME_HEADER_SIZE - 1U] = mark4::Transport::MAX_HOPS;
    inject(bus2, 0U, 1U, tired);
    const std::uint32_t relayedBefore = r.relayed();
    const std::uint32_t broadcastsR1 = linkR1.broadcasts();
    seenByR.poll(r, T0_US);
    CHECK(seenByR.delivered.size() == deliveredToR + 1U);
    CHECK(r.dropped() == droppedBefore + 2U);
    CHECK(r.relayed() == relayedBefore);
    CHECK(linkR1.broadcasts() == broadcastsR1);
}

TEST_CASE("a relay forwards a board broadcast to the lan exactly once and ignores its echo")
{
    // board --bus1 (the UART)-- hub --bus2 (the LAN)-- sim. The hub has two
    // links here, so it relays: the LAN echoes every broadcast back to its
    // sender, the UART does not.
    FakeBus uart;
    FakeBus lan;
    FakeLink linkBoard(uart);
    FakeLink linkHubUart(uart);
    FakeLink linkHubLan(lan);
    FakeLink linkSim(lan);
    mark4::Transport board(NODE_A, BOOT_ID);
    mark4::Transport hub(0x4B000000U, BOOT_ID);
    mark4::Transport sim(NODE_C, BOOT_ID);
    REQUIRE(board.addLink(linkBoard));
    REQUIRE(hub.addLink(linkHubUart));
    REQUIRE(hub.addLink(linkHubLan));
    REQUIRE(sim.addLink(linkSim));
    Observer seenByBoard(board);
    Observer seenByHub(hub);
    Observer seenBySim(sim);

    // Board status: delivered to the hub once, on the LAN once (beside the
    // hub's own keepalive, which is all the UART gets), to the sim once.
    // The hub then hears its own forwarding come back on the LAN.
    REQUIRE(board.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    REQUIRE(seenByHub.delivered.size() == 1U);
    CHECK(linkHubLan.broadcasts() == 2U);
    CHECK(linkHubUart.broadcasts() == 1U);
    seenBySim.poll(sim, T0_US);
    REQUIRE(seenBySim.delivered.size() == 1U);
    CHECK(seenBySim.delivered[0].first == NODE_A);
    seenByHub.poll(hub, T0_US);
    CHECK(seenByHub.delivered.size() == 1U);
    CHECK(linkHubLan.broadcasts() == 2U);
    // The echo of its own forwarding is dropped as a duplicate of the board's
    // frame, and counted as one: the transport cannot tell it from a loop.
    // (A UdpLink filters its own echoes before they get here; this fake
    // medium does not, so the count shows.)
    REQUIRE(hub.findNode(NODE_A) != nullptr);
    CHECK(hub.findNode(NODE_A)->duplicates == 1U);
    CHECK(hub.findNode(NODE_A)->received == 1U);

    // The keepalives settle: everyone knows everyone, nothing delivered.
    settle({{&board, &seenByBoard}, {&hub, &seenByHub}, {&sim, &seenBySim}}, {&uart, &lan}, T0_US);
    CHECK(seenByHub.delivered.size() == 1U);
    CHECK(seenBySim.delivered.size() == 1U);
    CHECK(seenByBoard.delivered.empty());
    CHECK(board.isAlive(NODE_C));
    CHECK(sim.isAlive(NODE_A));
    const std::uint32_t uartBroadcasts = linkHubUart.broadcasts();
    const std::uint32_t uartSent = linkHubUart.sent();
    const std::uint32_t lanSent = linkHubLan.sent();

    // The other way: a LAN broadcast reaches the board through the UART.
    REQUIRE(sim.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(seenByHub.delivered.size() == 2U);
    CHECK(linkHubUart.broadcasts() == uartBroadcasts + 1U);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == 1U);
    CHECK(seenByBoard.delivered[0].first == NODE_C);

    // A command from the sim to the board crosses to the UART; the board's
    // unicast answer crosses back.
    REQUIRE(sim.send(NODE_A, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(linkHubUart.sent() == uartSent + 1U);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == 2U);
    REQUIRE(board.send(NODE_C, HELLO.data(), HELLO.size()));
    seenByHub.poll(hub, T0_US);
    CHECK(linkHubLan.sent() == lanSent + 1U);
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

} // namespace

TEST_CASE("a relay carries every broadcast across and unicasts towards the board")
{
    // board --uart-- relay (ESP32: two links) --lan-- hub, sim.
    FakeBus uart;
    FakeBus lan;
    FakeLink linkBoard(uart);
    FakeLink linkRelayUart(uart);
    FakeLink linkRelayLan(lan);
    FakeLink linkHub(lan);
    FakeLink linkSim(lan);
    mark4::Transport board(NODE_A, BOOT_ID);
    mark4::Transport relay(0xE5320000U, BOOT_ID);
    mark4::Transport hub(NODE_B, BOOT_ID);
    mark4::Transport sim(NODE_C, BOOT_ID);
    REQUIRE(board.addLink(linkBoard));
    REQUIRE(relay.addLink(linkRelayUart));
    REQUIRE(relay.addLink(linkRelayLan));
    REQUIRE(hub.addLink(linkHub));
    REQUIRE(sim.addLink(linkSim));
    Observer seenByBoard(board);
    Observer seenByRelay(relay);
    Observer seenByHub(hub);
    Observer seenBySim(sim);

    const std::vector<std::uint8_t> status = envelopeBytes(mark4_Envelope_status_tag);
    const std::vector<std::uint8_t> command = envelopeBytes(mark4_Envelope_rc_tag);

    // Keepalives: every node's first poll broadcasts one, and the relay
    // carries each across, beside its own on both links.
    seenByBoard.poll(board, T0_US);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayLan.broadcasts() == 2U);  // the board's, and the relay's own
    CHECK(linkRelayUart.broadcasts() == 3U); // the hub's, the sim's, and its own
    CHECK(relay.relayed() == 3U);
    // Everyone learns the board from the relayed frame, one relay away, and
    // the board learns everyone; the relay is everyone's direct neighbour.
    // Nobody is delivered anything: the whole exchange is keepalives.
    seenByBoard.poll(board, T0_US);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    CHECK(hub.isAlive(NODE_A));
    CHECK(sim.isAlive(NODE_A));
    CHECK(board.isAlive(NODE_B));
    CHECK(board.isAlive(NODE_C));
    CHECK(hub.isAlive(relay.nodeId()));
    CHECK(hub.nodeCount() == 3U);
    REQUIRE(hub.findNode(NODE_A) != nullptr);
    CHECK(hub.findNode(NODE_A)->hops == 1U);
    REQUIRE(hub.findNode(relay.nodeId()) != nullptr);
    CHECK(hub.findNode(relay.nodeId())->hops == 0U);
    REQUIRE(board.findNode(NODE_B) != nullptr);
    CHECK(board.findNode(NODE_B)->hops == 1U);
    // The learners greet the newcomers with unicast keepalives, which cross
    // the relay in both directions; once they have settled, still nothing
    // was delivered anywhere.
    settle({{&board, &seenByBoard}, {&relay, &seenByRelay}, {&hub, &seenByHub}, {&sim, &seenBySim}},
           {&uart, &lan},
           T0_US);
    CHECK(seenByBoard.delivered.empty());
    CHECK(seenByRelay.delivered.empty());
    CHECK(seenByHub.delivered.empty());
    CHECK(seenBySim.delivered.empty());
    // The echo of its own forwarding of the board's frames is a duplicate
    // for the relay (this fake LAN echoes; a UdpLink drops its own echoes).
    REQUIRE(relay.findNode(NODE_A) != nullptr);
    const std::uint32_t duplicatesBefore = relay.findNode(NODE_A)->duplicates;

    // The sim's status broadcast reaches the hub on the LAN and crosses to
    // the board through the UART, exactly once.
    const std::uint32_t uartBefore = linkRelayUart.broadcasts();
    const std::size_t boardBefore = seenByBoard.delivered.size();
    REQUIRE(sim.send(mark4::BROADCAST_NODE, status.data(), status.size()));
    seenByRelay.poll(relay, T0_US);
    seenByHub.poll(hub, T0_US);
    seenByBoard.poll(board, T0_US);
    CHECK(seenByHub.delivered.back().second == status);
    CHECK(linkRelayUart.broadcasts() == uartBefore + 1U);
    REQUIRE(seenByBoard.delivered.size() == boardBefore + 1U);
    CHECK(seenByBoard.delivered.back().first == NODE_C);
    CHECK(seenByBoard.delivered.back().second == status);
    CHECK(relay.dropped() == 0U);

    // A command from the hub to the board is a unicast: it crosses.
    REQUIRE(hub.send(NODE_A, command.data(), command.size()));
    seenByRelay.poll(relay, T0_US);
    seenByBoard.poll(board, T0_US);
    REQUIRE(seenByBoard.delivered.size() == boardBefore + 2U);
    CHECK(seenByBoard.delivered.back().first == NODE_B);
    CHECK(seenByBoard.delivered.back().second == command);

    // The board's status broadcast reaches the LAN exactly once, with
    // one hop more, and both LAN nodes get it once.
    const std::uint32_t lanBefore = linkRelayLan.broadcasts();
    const std::size_t hubBefore = seenByHub.delivered.size();
    const std::size_t simBefore = seenBySim.delivered.size();
    REQUIRE(board.send(mark4::BROADCAST_NODE, status.data(), status.size()));
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayLan.broadcasts() == lanBefore + 1U);
    // The copy waiting in the hub's inbox (LAN endpoint 1) has one hop more.
    mark4::FrameHeader forwarded;
    REQUIRE(!lan.inbox[1].empty());
    REQUIRE(mark4::decodeFrameHeader(
        lan.inbox[1].back().bytes.data(), lan.inbox[1].back().bytes.size(), forwarded));
    CHECK(forwarded.hops == 1U);
    seenByHub.poll(hub, T0_US);
    seenBySim.poll(sim, T0_US);
    CHECK(seenByHub.delivered.size() == hubBefore + 1U);
    CHECK(seenBySim.delivered.size() == simBefore + 1U);
    CHECK(seenByHub.delivered.back().second == status);
    // The echo of its own forwarding is one more duplicate for the relay,
    // and it is not forwarded back onto the UART.
    seenByRelay.poll(relay, T0_US);
    CHECK(linkRelayUart.broadcasts() == uartBefore + 1U);
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
    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport b(NODE_B, BOOT_ID);
    mark4::Transport c(NODE_C, BOOT_ID);
    REQUIRE(a.addLink(aOnAB));
    REQUIRE(a.addLink(aOnCA));
    REQUIRE(b.addLink(bOnAB));
    REQUIRE(b.addLink(bOnBC));
    REQUIRE(c.addLink(cOnBC));
    REQUIRE(c.addLink(cOnCA));
    Observer seenByA(a);
    Observer seenByB(b);
    Observer seenByC(c);

    // The keepalives first: they flood the triangle like any broadcast and
    // settle, which is already the property under test.
    settle({{&a, &seenByA}, {&b, &seenByB}, {&c, &seenByC}}, {&busAB, &busBC, &busCA}, T0_US);
    CHECK(seenByA.delivered.empty());
    CHECK(seenByB.delivered.empty());
    CHECK(seenByC.delivered.empty());
    const std::uint32_t broadcastsA = aOnAB.broadcasts() + aOnCA.broadcasts();
    const std::uint32_t broadcastsB = bOnBC.broadcasts();
    const std::uint32_t broadcastsC = cOnCA.broadcasts();

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
    CHECK(aOnAB.broadcasts() + aOnCA.broadcasts() == broadcastsA + 2U);
    CHECK(bOnBC.broadcasts() == broadcastsB + 1U);
    CHECK(cOnCA.broadcasts() == broadcastsC + 1U);
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
    CHECK(std::holds_alternative<mark4::UartAddress>(from));
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
    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport b(NODE_B, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    Observer seenByB(b);
    REQUIRE(a.send(mark4::BROADCAST_NODE, HELLO.data(), HELLO.size()));
    seenByB.poll(b, T0_US);
    REQUIRE(seenByB.delivered.size() == 1U);
    CHECK(seenByB.delivered[0].first == NODE_A);
    CHECK(seenByB.delivered[0].second == HELLO);
    REQUIRE(b.send(NODE_A, HELLO.data(), HELLO.size()));
    Observer seenByA(a);
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

    mark4::Transport a(NODE_A, BOOT_ID);
    mark4::Transport b(NODE_B, BOOT_ID);
    REQUIRE(a.addLink(linkA));
    REQUIRE(b.addLink(linkB));
    REQUIRE(a.init());
    REQUIRE(b.init());
    Observer seenByA(a);
    Observer seenByB(b);

    // Both keepalives are broadcast on the first poll; each side learns the
    // other, and nothing is delivered. A sandbox that forbids every
    // broadcast route fails here, and that is the signal to read: nothing
    // else in this test can work.
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
    const auto *addressB = std::get_if<mark4::UdpAddress>(&a.findNode(NODE_B)->address);
    REQUIRE(addressB != nullptr);
    CHECK(addressB->port == linkB.dataPort());
    CHECK(seenByA.delivered.empty());

    // Unicast by id goes to the data socket of the peer.
    REQUIRE(a.send(NODE_B, HELLO.data(), HELLO.size()));
    REQUIRE(pollUntil(b, seenByB, [&seenByB] {
        return !seenByB.delivered.empty() && seenByB.delivered.back().second == HELLO;
    }));
    CHECK(seenByB.delivered.back().first == NODE_A);

    // A node on another discovery port is on another deployment: unheard.
    mark4::UdpLink linkC(pickFreePort());
    REQUIRE(linkC.init());
    mark4::Transport c(NODE_C, BOOT_ID);
    REQUIRE(c.addLink(linkC));
    Observer seenByC(c);
    seenByC.poll(c, T0_US);
    CHECK(!pollUntil(a, seenByA, [&a] { return a.isAlive(NODE_C); }));
}
