/// @file
/// @brief The messenger over a recording link: dispatch by body tag,
///        self-registering handlers, the tap, the counters, send() as the
///        one encoder, and the keepalive that never reaches it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "recording_link.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 10'000'000U;
    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D00U;
    /// Requests these benches keep at once: enough for what one test
    /// exchanges, the size a board's composition uses.
    constexpr std::size_t PENDING_REQUESTS = mark4::Messenger::BOARD_PENDING_REQUESTS;
    constexpr std::uint32_t NODE_ME = 0xA0000001U;
    constexpr std::uint32_t NODE_PEER = 0xB0000002U;
    constexpr float THROTTLE = 0.25f;

    constexpr std::array<pb_size_t, 1> RC_TAGS{mark4_Envelope_rc_tag};
    constexpr std::array<pb_size_t, 2> RC_AND_STATUS_TAGS{mark4_Envelope_rc_tag,
                                                          mark4_Envelope_status_tag};

    /// One call a handler or the tap saw.
    struct Seen
    {
        std::uint32_t src = 0U;   ///< node the message came from
        pb_size_t tag = 0U;       ///< body tag
        std::uint64_t nowUs = 0U; ///< instant of the poll
        float throttle = 0.0f;    ///< the Rc throttle when the body is an Rc
    };

    /// Handler logging what it receives, answering what it is told to.
    class Probe final : public mark4::AbsMessageHandler
    {
      public:
        Probe(mark4::Messenger &messenger,
              std::span<const pb_size_t> tags,
              std::vector<std::string> *events = nullptr)
            : AbsMessageHandler(messenger, tags),
              m_messenger(messenger),
              m_events(events)
        {
        }

        bool onMessage(std::uint32_t src,
                       const mark4_Envelope &envelope,
                       std::uint64_t nowUs) override
        {
            Seen seen;
            seen.src = src;
            seen.tag = envelope.which_body;
            seen.nowUs = nowUs;
            if (envelope.which_body == mark4_Envelope_rc_tag)
            {
                seen.throttle = envelope.body.rc.throttle;
            }
            m_seen.push_back(seen);
            if (m_events != nullptr)
            {
                m_events->emplace_back("handler");
            }
            if (m_answer)
            {
                mark4_Envelope answer = mark4_Envelope_init_zero;
                answer.which_body = mark4_Envelope_status_tag;
                m_messenger.send(src, answer);
            }
            return m_accept;
        }

        /// @param accept what onMessage() returns from now on
        void setAccept(bool accept)
        {
            m_accept = accept;
        }

        /// @param answer true to send a Status back to src on every message
        void setAnswer(bool answer)
        {
            m_answer = answer;
        }

        [[nodiscard]] const std::vector<Seen> &seen() const
        {
            return m_seen;
        }

      private:
        mark4::Messenger &m_messenger;      ///< to answer from inside onMessage()
        std::vector<std::string> *m_events; ///< shared order log, may be nullptr
        std::vector<Seen> m_seen;           ///< everything received
        bool m_accept = true;               ///< what onMessage() returns
        bool m_answer = false;              ///< whether it answers
    };

    /// What the tap records.
    struct TapLog
    {
        std::vector<Seen> seen;                       ///< src of every payload, in order
        std::vector<std::vector<std::uint8_t>> bytes; ///< the raw bytes of each
        std::vector<std::string> *events = nullptr;   ///< shared order log
    };

    void tap(void *context, std::uint32_t src, const std::uint8_t *payload, std::size_t size)
    {
        auto *log = static_cast<TapLog *>(context);
        Seen seen;
        seen.src = src;
        log->seen.push_back(seen);
        log->bytes.emplace_back(payload, payload + size);
        if (log->events != nullptr)
        {
            log->events->emplace_back("tap");
        }
    }

    /// @param throttle Rc throttle
    /// @return an Rc envelope
    mark4_Envelope rcEnvelope(float throttle)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_rc_tag;
        envelope.body.rc.throttle = throttle;
        return envelope;
    }

    /// @return a Status envelope
    mark4_Envelope statusEnvelope()
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_status_tag;
        envelope.body.status.throw_count = 7U;
        return envelope;
    }

    /// @param envelope message to encode
    /// @return its wire bytes
    std::vector<std::uint8_t> encode(const mark4_Envelope &envelope)
    {
        std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> buffer{};
        std::size_t size = 0U;
        REQUIRE(mark4::encodeEnvelope(envelope, buffer.data(), buffer.size(), size));
        return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size)};
    }

    /// Bytes no Envelope decoder accepts: a tag varint that never ends.
    const std::vector<std::uint8_t> GARBAGE{0xFFU, 0xFFU, 0xFFU};

    /// One node with a recording link and a messenger on top.
    struct Bench
    {
        mark4::RecordingLink link;                                     ///< the medium
        mark4::Transport transport{NODE_ME, BOOT_ID};                  ///< this node
        std::array<mark4::PendingRequest, PENDING_REQUESTS> pending{}; ///< requests kept
        mark4::Messenger messenger{transport, pending};                ///< under test

        Bench()
        {
            REQUIRE(transport.addLink(link));
            REQUIRE(transport.init());
        }

        /// @brief Makes the transport hear the peer once, so it can be
        ///        unicast to, and forgets the keepalive that greets it.
        void learnPeer()
        {
            link.deliver(NODE_PEER, NODE_ME, encode(statusEnvelope()));
            messenger.poll(T0_US);
            link.clear();
        }
    };
} // namespace

TEST_CASE("messenger dispatches a decoded message to the handler of its tag", "[messaging]")
{
    Bench bench;
    Probe rc(bench.messenger, RC_TAGS);
    REQUIRE(bench.messenger.init());

    bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
    bench.link.deliver(NODE_PEER, NODE_ME, encode(statusEnvelope()));
    bench.link.deliver(NODE_PEER, NODE_ME, GARBAGE);
    bench.messenger.poll(T0_US);

    REQUIRE(rc.seen().size() == 1U);
    CHECK(rc.seen()[0].src == NODE_PEER);
    CHECK(rc.seen()[0].tag == mark4_Envelope_rc_tag);
    CHECK(rc.seen()[0].nowUs == T0_US);
    CHECK(rc.seen()[0].throttle == THROTTLE);
    CHECK(bench.messenger.received() == 3U);
    CHECK(bench.messenger.handled() == 1U);
    CHECK(bench.messenger.unhandled() == 1U);
    CHECK(bench.messenger.undecodable() == 1U);
    CHECK(bench.messenger.ignored() == 0U);
}

TEST_CASE("messenger hands every claimed tag to its handler and counts what it ignores",
          "[messaging]")
{
    Bench bench;
    Probe both(bench.messenger, RC_AND_STATUS_TAGS);
    REQUIRE(bench.messenger.init());

    bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
    bench.link.deliver(NODE_PEER, NODE_ME, encode(statusEnvelope()));
    bench.messenger.poll(T0_US);

    REQUIRE(both.seen().size() == 2U);
    CHECK(both.seen()[0].tag == mark4_Envelope_rc_tag);
    CHECK(both.seen()[1].tag == mark4_Envelope_status_tag);
    CHECK(bench.messenger.handled() == 2U);
    CHECK(bench.messenger.ignored() == 0U);

    both.setAccept(false);
    bench.link.deliver(NODE_PEER, NODE_ME, encode(statusEnvelope()));
    bench.messenger.poll(T0_US + 1U);

    CHECK(both.seen().size() == 3U);
    CHECK(bench.messenger.handled() == 2U);
    CHECK(bench.messenger.ignored() == 1U);
    CHECK(bench.messenger.unhandled() == 0U);
}

TEST_CASE("messenger refuses init while two handlers claim one tag", "[messaging]")
{
    Bench bench;
    std::optional<Probe> first;
    std::optional<Probe> second;
    first.emplace(bench.messenger, RC_TAGS);
    second.emplace(bench.messenger, RC_TAGS);
    CHECK(!bench.messenger.init());

    SECTION("the shadowed one goes away: the owner keeps its slot")
    {
        second.reset();
        CHECK(bench.messenger.init());

        bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
        bench.messenger.poll(T0_US);
        CHECK(first->seen().size() == 1U);
        CHECK(bench.messenger.handled() == 1U);
    }

    SECTION("the owner goes away: the shadowed one never claimed the slot")
    {
        // The survivor does not take over: it is alive and hears nothing, so
        // init() keeps reporting it until it goes away too.
        first.reset();
        CHECK(!bench.messenger.init());

        bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
        bench.messenger.poll(T0_US);
        CHECK(second->seen().empty());
        CHECK(bench.messenger.handled() == 0U);
        CHECK(bench.messenger.unhandled() == 1U);

        second.reset();
        CHECK(bench.messenger.init());
    }
}

TEST_CASE("messenger frees the slot of a destroyed handler", "[messaging]")
{
    Bench bench;
    std::optional<Probe> first;
    first.emplace(bench.messenger, RC_TAGS);
    REQUIRE(bench.messenger.init());
    first.reset();

    bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
    bench.messenger.poll(T0_US);
    CHECK(bench.messenger.unhandled() == 1U);

    Probe replacement(bench.messenger, RC_TAGS);
    REQUIRE(bench.messenger.init());
    bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
    bench.messenger.poll(T0_US + 1U);
    CHECK(replacement.seen().size() == 1U);
    CHECK(bench.messenger.handled() == 1U);
}

TEST_CASE("messenger tap sees the raw bytes of every payload before dispatch", "[messaging]")
{
    Bench bench;
    std::vector<std::string> events;
    Probe rc(bench.messenger, RC_TAGS, &events);
    REQUIRE(bench.messenger.init());

    SECTION("with a tap")
    {
        TapLog log;
        log.events = &events;
        bench.messenger.setTap(&tap, &log);

        const std::vector<std::uint8_t> rcBytes = encode(rcEnvelope(THROTTLE));
        bench.link.deliver(NODE_PEER, NODE_ME, rcBytes);
        bench.link.deliver(NODE_PEER, NODE_ME, GARBAGE);
        bench.messenger.poll(T0_US);

        REQUIRE(log.seen.size() == 2U);
        CHECK(log.seen[0].src == NODE_PEER);
        CHECK(log.bytes[0] == rcBytes);
        CHECK(log.seen[1].src == NODE_PEER);
        CHECK(log.bytes[1] == GARBAGE);
        CHECK(events == std::vector<std::string>{"tap", "handler", "tap"});
        CHECK(rc.seen().size() == 1U);
        CHECK(bench.messenger.undecodable() == 1U);
    }

    SECTION("without a tap")
    {
        bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
        bench.link.deliver(NODE_PEER, NODE_ME, GARBAGE);
        bench.messenger.poll(T0_US);

        CHECK(events == std::vector<std::string>{"handler"});
        CHECK(bench.messenger.received() == 2U);
    }
}

TEST_CASE("messenger send encodes one unicast frame and refuses the rest", "[messaging]")
{
    Bench bench;
    bench.learnPeer();

    CHECK(bench.messenger.send(NODE_PEER, rcEnvelope(THROTTLE)));
    REQUIRE(bench.link.frames().size() == 1U);
    CHECK(bench.link.frames()[0].header.src == NODE_ME);
    CHECK(bench.link.frames()[0].header.dst == NODE_PEER);
    CHECK(!bench.link.frames()[0].broadcast);
    const std::optional<mark4_Envelope> sent = bench.link.envelope(0U);
    REQUIRE(sent.has_value());
    CHECK(sent->which_body == mark4_Envelope_rc_tag);
    CHECK(sent->body.rc.throttle == THROTTLE);
    CHECK(bench.messenger.sent() == 1U);
    CHECK(bench.messenger.refused() == 0U);

    CHECK(!bench.messenger.send(mark4::BROADCAST_NODE, rcEnvelope(THROTTLE)));
    CHECK(bench.link.frames().size() == 1U);
    CHECK(bench.messenger.refused() == 1U);

    CHECK(!bench.messenger.send(NODE_PEER + 1U, rcEnvelope(THROTTLE)));
    CHECK(bench.link.frames().size() == 1U);
    CHECK(bench.messenger.refused() == 2U);
    CHECK(bench.messenger.sent() == 1U);
}

TEST_CASE("messenger handler answers from inside onMessage", "[messaging]")
{
    Bench bench;
    bench.learnPeer();
    Probe rc(bench.messenger, RC_TAGS);
    rc.setAnswer(true);
    REQUIRE(bench.messenger.init());

    bench.link.deliver(NODE_PEER, NODE_ME, encode(rcEnvelope(THROTTLE)));
    bench.messenger.poll(T0_US + 1U);

    REQUIRE(bench.link.frames().size() == 1U);
    CHECK(bench.link.frames()[0].header.dst == NODE_PEER);
    const std::optional<mark4_Envelope> answer = bench.link.envelope(0U);
    REQUIRE(answer.has_value());
    CHECK(answer->which_body == mark4_Envelope_status_tag);
    CHECK(bench.messenger.sent() == 1U);
    CHECK(bench.messenger.handled() == 1U);
}

TEST_CASE("messenger never sees the transport keepalive", "[messaging]")
{
    Bench bench;
    Probe rc(bench.messenger, RC_TAGS);
    REQUIRE(bench.messenger.init());

    bench.link.deliver(NODE_PEER, NODE_ME, {});
    bench.messenger.poll(T0_US);
    bench.link.deliver(NODE_PEER, mark4::BROADCAST_NODE, {});
    bench.messenger.poll(T0_US + 1U);

    CHECK(bench.transport.isAlive(NODE_PEER));
    CHECK(bench.messenger.received() == 0U);
    CHECK(rc.seen().empty());
}
