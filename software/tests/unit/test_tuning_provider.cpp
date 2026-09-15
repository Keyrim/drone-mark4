/// @file
/// @brief The tuning provider, driven through the wire: which messages it
///        claims, what it answers with, and to whom.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/flight_core.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/tuning_table.hpp"
#include "flight_core/types.hpp"
#include "messaging/messenger.hpp"
#include "protocol/envelope.hpp"
#include "recording_link.hpp"
#include "transport/frame.hpp"
#include "transport/transport.hpp"
#include "tuning/provider.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream
    constexpr float HELPER_BARO_PA = 101325.0f;

    /// Incarnation every transport of this file is built with: a test
    /// restarts nothing, so one constant stands for the random draw.
    constexpr std::uint32_t BOOT_ID = 0xB0071D00U;
    /// Requests these benches keep at once. The gateway's size, because
    /// nothing here acknowledges what the provider answers and every
    /// unanswered request holds an entry.
    constexpr std::size_t PENDING_REQUESTS = mark4::Messenger::HUB_PENDING_REQUESTS;
    constexpr std::uint32_t NODE_SELF = 0x51A17000U;
    constexpr std::uint32_t NODE_GROUND = 0x67000001U;
    constexpr std::uint32_t NODE_OTHER = 0x67000002U;
    constexpr std::uint64_t T0_US = 1'000'000U;

    /// A messenger over a transport over a recording link: the requests
    /// come in through the link, the provider answers on it, and the test
    /// reads back the payloads and where they went.
    class Wire
    {
      public:
        Wire()
        {
            static_cast<void>(m_transport.addLink(m_link));
            // The two requesters of the tests are learnt up front, so a
            // unicast to either can leave: a node nobody ever heard from has
            // no address and every send to it is refused.
            learn(NODE_GROUND);
            learn(NODE_OTHER);
        }

        /// @return messenger the provider under test attaches to
        mark4::Messenger &messenger()
        {
            return m_messenger;
        }

        /// @brief Delivers one message to this node and polls once, the way
        ///        a flight loop does.
        /// @param envelope message to deliver
        /// @param src node it comes from
        /// @return true when a handler acted on it
        bool request(const mark4_Envelope &envelope, std::uint32_t src = NODE_GROUND)
        {
            std::vector<std::uint8_t> bytes(mark4::MAX_ENVELOPE_SIZE, 0U);
            std::size_t size = 0U;
            REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
            bytes.resize(size);
            m_link.deliver(src, NODE_SELF, bytes);
            const std::uint32_t handled = m_messenger.handled();
            m_messenger.poll(T0_US);
            return m_messenger.handled() == handled + 1U;
        }

        /// @return messages the messenger found no handler for
        [[nodiscard]] std::uint32_t unhandled() const
        {
            return m_messenger.unhandled();
        }

        /// @return payload of every frame sent so far, in order
        [[nodiscard]] std::vector<std::vector<std::uint8_t>> sent() const
        {
            std::vector<std::vector<std::uint8_t>> payloads;
            payloads.reserve(m_link.frames().size());
            for (const mark4::RecordedFrame &frame : m_link.frames())
            {
                payloads.push_back(frame.payload);
            }
            return payloads;
        }

        /// @return every frame sent so far, headers included
        [[nodiscard]] const std::vector<mark4::RecordedFrame> &frames() const
        {
            return m_link.frames();
        }

        /// @brief Forgets everything recorded so far.
        void clear()
        {
            m_link.clear();
        }

      private:
        /// @brief Makes the transport learn one node.
        /// @param node node to learn
        void learn(std::uint32_t node)
        {
            m_link.deliver(node, NODE_SELF, {0x00U});
            m_messenger.poll(T0_US);
            REQUIRE(m_transport.isAlive(node));
            m_link.clear();
        }

        mark4::RecordingLink m_link;                                     ///< the medium
        mark4::Transport m_transport{NODE_SELF, BOOT_ID};                ///< this node
        std::array<mark4::PendingRequest, PENDING_REQUESTS> m_pending{}; ///< requests kept
        mark4::Messenger m_messenger{m_transport, m_pending}; ///< what the provider attaches to
    };

    /// @param id parameter id
    /// @param value requested value
    /// @return one TuningSet
    mark4_Envelope makeSet(std::uint32_t id, float value)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_set_tag;
        envelope.body.tuning_set.id = id;
        envelope.body.tuning_set.value = value;
        return envelope;
    }

    /// @param id parameter id
    /// @return one TuningGet
    mark4_Envelope makeGet(std::uint32_t id)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_get_tag;
        envelope.body.tuning_get.id = id;
        return envelope;
    }

    /// @param cursor first table index to describe
    /// @return one TuningListRequest
    mark4_Envelope makeList(std::uint32_t cursor)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_list_request_tag;
        envelope.body.tuning_list_request.cursor = cursor;
        return envelope;
    }

    /// @param datagram bytes to decode
    /// @return the envelope the datagram carries
    mark4_Envelope decode(const std::vector<std::uint8_t> &datagram)
    {
        mark4_Envelope envelope;
        REQUIRE(mark4::decodeEnvelope(datagram.data(), datagram.size(), envelope));
        return envelope;
    }

    /// @param datagram bytes to decode
    /// @return the ack the datagram carries
    mark4_TuningAck decodeAck(const std::vector<std::uint8_t> &datagram)
    {
        const mark4_Envelope envelope = decode(datagram);
        REQUIRE(envelope.which_body == mark4_Envelope_tuning_ack_tag);
        return envelope.body.tuning_ack;
    }

    /// @param datagram bytes to decode
    /// @return the page of parameter descriptions the datagram carries
    mark4_TuningInfos decodePage(const std::vector<std::uint8_t> &datagram)
    {
        const mark4_Envelope envelope = decode(datagram);
        REQUIRE(envelope.which_body == mark4_Envelope_tuning_infos_tag);
        return envelope.body.tuning_infos;
    }

    /// @brief Drives a core to ARMED: settled on the ground, altitude-auto
    ///        selected, stick centered, arm switch on.
    /// @param core core to drive
    void driveToArmed(mark4::FlightCore &core)
    {
        mark4::ActuatorFrame actuators;
        mark4::SensorFrame frame;
        frame.rc.killSwitch = false;
        frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
        frame.rc.throttle = 0.5f;
        frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
        frame.baroPa = HELPER_BARO_PA;
        frame.imuValid = true;
        frame.baroValid = true;
        for (std::uint32_t i = 0U; i < 200U; ++i)
        {
            frame.timestampUs = static_cast<std::uint64_t>(i + 1U) * STEP_US;
            frame.rc.armSwitch = i >= 100U;
            core.step(frame, actuators);
        }
        REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
    }
} // namespace

TEST_CASE("a tuning set is applied and acknowledged with the value in effect")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    REQUIRE(wire.request(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.7f)));
    REQUIRE(provider.requestCount() == 1U);
    REQUIRE(wire.sent().size() == 1U);

    const mark4_TuningAck ack = decodeAck(wire.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_HOVER_COLLECTIVE);
    REQUIRE(ack.status == mark4_TuningStatus_OK);
    REQUIRE(ack.value == 0.7f);

    // The value the ack carries is the one the core is really flying.
    float live = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, live) == mark4::TuningStatus::OK);
    REQUIRE(live == 0.7f);
}

TEST_CASE("an out-of-bounds tuning set is refused and the live value survives")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    float before = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, before) == mark4::TuningStatus::OK);

    REQUIRE(wire.request(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 42.0f)));
    const mark4_TuningAck ack = decodeAck(wire.sent()[0]);
    REQUIRE(ack.status == mark4_TuningStatus_OUT_OF_BOUNDS);
    // The refusal answers with what is still flying, not with what was asked.
    REQUIRE(ack.value == before);
}

TEST_CASE("an unknown parameter id is acknowledged as unknown")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    REQUIRE(wire.request(makeSet(9999U, 1.0f)));
    const mark4_TuningAck ack = decodeAck(wire.sent()[0]);
    REQUIRE(ack.id == 9999U);
    REQUIRE(ack.status == mark4_TuningStatus_UNKNOWN_ID);
    REQUIRE(ack.value == 0.0f);
}

TEST_CASE("a parameter locked while armed is refused with its own status")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);
    driveToArmed(core);

    REQUIRE(wire.request(makeSet(mark4::TUNING_ID_AHRS_KP, 3.0f)));
    const mark4_TuningAck ack = decodeAck(wire.sent()[0]);
    REQUIRE(ack.status == mark4_TuningStatus_LOCKED_WHILE_ARMED);
    REQUIRE(ack.value == mark4::AttitudeEstimator::DEFAULT_KP);

    // A gain a pilot does retune between throws goes through.
    wire.clear();
    REQUIRE(wire.request(makeSet(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.05f)));
    REQUIRE(decodeAck(wire.sent()[0]).status == mark4_TuningStatus_OK);
}

TEST_CASE("a tuning get reads a value back without changing it")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);
    REQUIRE(core.setParam(mark4::TUNING_ID_VERTICAL_KP, 0.42f) == mark4::TuningStatus::OK);

    REQUIRE(wire.request(makeGet(mark4::TUNING_ID_VERTICAL_KP)));
    const mark4_TuningAck ack = decodeAck(wire.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_VERTICAL_KP);
    REQUIRE(ack.status == mark4_TuningStatus_OK);
    REQUIRE(ack.value == 0.42f);

    REQUIRE(wire.request(makeGet(1234U)));
    REQUIRE(decodeAck(wire.sent()[1]).status == mark4_TuningStatus_UNKNOWN_ID);
}

TEST_CASE("a tuning list is answered one page per request, in table order")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    // One page per request: the requester paces the walk, and the last page
    // is the one where cursor + infos == total.
    std::size_t listed = 0U;
    std::size_t pages = 0U;
    do
    {
        REQUIRE(wire.request(makeList(static_cast<std::uint32_t>(listed))));
        REQUIRE(wire.sent().size() == pages + 1U);
        const mark4_TuningInfos page = decodePage(wire.sent()[pages]);
        REQUIRE(page.total == mark4::FlightCore::ParamCount());
        REQUIRE(page.cursor == listed);
        REQUIRE(page.infos_count > 0U);
        for (pb_size_t index = 0U; index < page.infos_count; ++index)
        {
            const std::size_t at = listed + index;
            INFO("entry " << at);
            // Everything the description carries must match the registry
            // entry it describes: the ground side reads this instead of a
            // table of its own, so a mismatch here is a silently wrong
            // ground station.
            const mark4::TuningParam *param = core.paramInfo(at);
            REQUIRE(param != nullptr);
            const mark4_TuningInfo &info = page.infos[index];
            REQUIRE(info.id == param->id);
            REQUIRE(info.value == param->value);
            REQUIRE(info.min_value == param->minValue);
            REQUIRE(info.max_value == param->maxValue);
            REQUIRE(info.armed_change == param->armedChange);

            const std::string name(info.name);
            REQUIRE(!name.empty());
            REQUIRE(name.size() <= mark4::TuningParam::NAME_SIZE);
        }
        listed += page.infos_count;
        ++pages;
    } while (listed < mark4::FlightCore::ParamCount());
    REQUIRE(listed == mark4::FlightCore::ParamCount());
    // A table of a dozen parameters is a handful of pages, not one frame per
    // parameter.
    REQUIRE(pages < mark4::FlightCore::ParamCount());
}

TEST_CASE("a tuning list starting past the end answers an empty page")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    REQUIRE(wire.request(makeList(static_cast<std::uint32_t>(mark4::FlightCore::ParamCount()))));
    REQUIRE(wire.sent().size() == 1U);
    const mark4_TuningInfos page = decodePage(wire.sent()[0]);
    REQUIRE(page.infos_count == 0U);
    REQUIRE(page.total == mark4::FlightCore::ParamCount());
}

TEST_CASE("a tuning list asks again wherever a lost page left it")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    REQUIRE(wire.request(makeList(0U)));
    REQUIRE(wire.sent().size() == 1U);
    REQUIRE(decodePage(wire.sent()[0]).cursor == 0U);

    // A second request positions the walk wherever it asks: this is how a
    // ground station asks again for the entries a lost frame took with it.
    wire.clear();
    REQUIRE(wire.request(makeList(3U)));
    REQUIRE(wire.sent().size() == 1U);
    const mark4_TuningInfos page = decodePage(wire.sent()[0]);
    REQUIRE(page.cursor == 3U);
    const mark4::TuningParam *param = core.paramInfo(3U);
    REQUIRE(param != nullptr);
    REQUIRE(page.infos[0].id == param->id);
}

TEST_CASE("a message that is not a tuning request is left to its owner")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    // Nothing claimed the tag: the messenger counts it, the provider never
    // sees it and answers nothing.
    mark4_Envelope reboot = mark4_Envelope_init_zero;
    reboot.which_body = mark4_Envelope_reboot_tag;
    REQUIRE(!wire.request(reboot));
    REQUIRE(wire.unhandled() == 1U);

    REQUIRE(provider.requestCount() == 0U);
    REQUIRE(wire.sent().empty());
}

TEST_CASE("the tuning answers go to the node that asked")
{
    mark4::FlightCore core;
    Wire wire;
    mark4::TuningProvider provider(wire.messenger(), core);

    // A request is a conversation with one node: the ack goes back to it,
    // unicast, and to nobody else.
    REQUIRE(wire.request(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.7f), NODE_OTHER));
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(!wire.frames()[0].broadcast);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
    REQUIRE(wire.frames()[0].header.src == NODE_SELF);

    wire.clear();
    REQUIRE(wire.request(makeGet(mark4::TUNING_ID_HOVER_COLLECTIVE), NODE_GROUND));
    REQUIRE(wire.frames()[0].header.dst == NODE_GROUND);

    // A page goes to whoever asked for it, like every other answer.
    wire.clear();
    REQUIRE(wire.request(makeList(0U), NODE_OTHER));
    REQUIRE(wire.frames().size() == 1U);
    REQUIRE(!wire.frames()[0].broadcast);
    REQUIRE(wire.frames()[0].header.dst == NODE_OTHER);
    REQUIRE(decodePage(wire.sent()[0]).cursor == 0U);
}
