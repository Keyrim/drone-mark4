/// @file
/// @brief The tuning adapter: which packets it claims, what it answers with,
///        and how it shares a command receiver with the RC tracker.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/flight_core.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/tuning_table.hpp"
#include "flight_core/types.hpp"
#include "platform/command_receiver.hpp"
#include "platform/telemetry_sender.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/tuning_service.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/tuning.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream
    constexpr float HELPER_BARO_PA = 101325.0f;

    /// Telemetry sender keeping every datagram handed to it, so a test can
    /// check the exact bytes that went out. Allocates freely: this is a test.
    class FakeTelemetrySender final : public mark4::AbsTelemetrySender
    {
      public:
        void send(const std::uint8_t *data, std::size_t size) override
        {
            m_sent.emplace_back(data, data + size);
        }

        /// @return datagrams captured since construction
        [[nodiscard]] const std::vector<std::vector<std::uint8_t>> &sent() const
        {
            return m_sent;
        }

        /// @brief Forgets everything captured so far.
        void clear()
        {
            m_sent.clear();
        }

      private:
        std::vector<std::vector<std::uint8_t>> m_sent; ///< captured datagrams
    };

    /// Command receiver handing out a scripted queue of packets, one per
    /// poll(). Allocates freely: this is a test, not flight code.
    class FakeCommandReceiver final : public mark4::AbsCommandReceiver
    {
      public:
        /// @brief Queues one packet to be handed out by a later poll().
        /// @param packet bytes of the packet
        void push(const std::vector<std::uint8_t> &packet)
        {
            m_queue.push_back(packet);
        }

        std::size_t poll(std::uint8_t *bufferOut, std::size_t capacity) override
        {
            if (m_queue.empty() || bufferOut == nullptr)
            {
                return 0U;
            }
            const std::vector<std::uint8_t> packet = m_queue.front();
            m_queue.pop_front();
            if (packet.size() > capacity)
            {
                return 0U;
            }
            std::memcpy(bufferOut, packet.data(), packet.size());
            return packet.size();
        }

      private:
        std::deque<std::vector<std::uint8_t>> m_queue; ///< packets still to hand out
    };

    /// @param id parameter id
    /// @param value requested value
    /// @return wire bytes of one TuningSetPacket
    std::vector<std::uint8_t> makeSet(std::uint16_t id, float value)
    {
        mark4::TuningSetPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_SET);
        packet.id = id;
        packet.value = value;
        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }

    /// @param id parameter id
    /// @return wire bytes of one TuningGetPacket
    std::vector<std::uint8_t> makeGet(std::uint16_t id)
    {
        mark4::TuningGetPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_GET);
        packet.id = id;
        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }

    /// @param startIndex first table index to describe
    /// @return wire bytes of one TuningListPacket
    std::vector<std::uint8_t> makeList(std::uint16_t startIndex)
    {
        mark4::TuningListPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_LIST);
        packet.startIndex = startIndex;
        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }

    /// @param datagram bytes to decode
    /// @return the ack the datagram carries
    mark4::TuningAckPacket decodeAck(const std::vector<std::uint8_t> &datagram)
    {
        REQUIRE(datagram.size() == mark4::TUNING_ACK_PACKET_SIZE);
        REQUIRE(mark4::hasHeader(datagram.data(), datagram.size(), mark4::PacketType::TUNING_ACK));
        mark4::TuningAckPacket packet{};
        std::memcpy(&packet, datagram.data(), sizeof(packet));
        return packet;
    }

    /// @param datagram bytes to decode
    /// @return the parameter description the datagram carries
    mark4::TuningInfoPacket decodeInfo(const std::vector<std::uint8_t> &datagram)
    {
        REQUIRE(datagram.size() == mark4::TUNING_INFO_PACKET_SIZE);
        REQUIRE(mark4::hasHeader(datagram.data(), datagram.size(), mark4::PacketType::TUNING_INFO));
        mark4::TuningInfoPacket packet{};
        std::memcpy(&packet, datagram.data(), sizeof(packet));
        return packet;
    }

    /// @brief Reads a zero-padded, possibly unterminated wire name.
    /// @param name name field of a description
    /// @return the name as a string, never running past the field
    std::string nameOf(const std::array<char, mark4::TUNING_NAME_SIZE> &name)
    {
        std::size_t length = 0U;
        while (length < name.size() && name[length] != '\0')
        {
            ++length;
        }
        return {name.data(), length};
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
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    const auto request = makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.7f);
    REQUIRE(service.handle(request.data(), request.size()));
    REQUIRE(service.requestCount() == 1U);
    REQUIRE(sender.sent().size() == 1U);

    const mark4::TuningAckPacket ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_HOVER_COLLECTIVE);
    REQUIRE(ack.status == mark4::TUNING_ACK_OK);
    REQUIRE(ack.value == 0.7f);

    // The value the ack carries is the one the core is really flying.
    float live = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, live) == mark4::TuningStatus::OK);
    REQUIRE(live == 0.7f);
}

TEST_CASE("an out-of-bounds tuning set is refused and the live value survives")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    float before = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, before) == mark4::TuningStatus::OK);

    const auto request = makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 42.0f);
    REQUIRE(service.handle(request.data(), request.size()));
    const mark4::TuningAckPacket ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.status == mark4::TUNING_ACK_OUT_OF_BOUNDS);
    // The refusal answers with what is still flying, not with what was asked.
    REQUIRE(ack.value == before);
}

TEST_CASE("an unknown parameter id is acknowledged as unknown")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    const auto request = makeSet(9999U, 1.0f);
    REQUIRE(service.handle(request.data(), request.size()));
    const mark4::TuningAckPacket ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == 9999U);
    REQUIRE(ack.status == mark4::TUNING_ACK_UNKNOWN_ID);
    REQUIRE(ack.value == 0.0f);
}

TEST_CASE("a parameter locked while armed is refused with its own status")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);
    driveToArmed(core);

    const auto request = makeSet(mark4::TUNING_ID_AHRS_KP, 3.0f);
    REQUIRE(service.handle(request.data(), request.size()));
    const mark4::TuningAckPacket ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.status == mark4::TUNING_ACK_LOCKED_WHILE_ARMED);
    REQUIRE(ack.value == mark4::AttitudeEstimator::DEFAULT_KP);

    // A gain a pilot does retune between throws goes through.
    sender.clear();
    const auto allowed = makeSet(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.05f);
    REQUIRE(service.handle(allowed.data(), allowed.size()));
    REQUIRE(decodeAck(sender.sent()[0]).status == mark4::TUNING_ACK_OK);
}

TEST_CASE("a tuning get reads a value back without changing it")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);
    REQUIRE(core.setParam(mark4::TUNING_ID_VERTICAL_KP, 0.42f) == mark4::TuningStatus::OK);

    const auto request = makeGet(mark4::TUNING_ID_VERTICAL_KP);
    REQUIRE(service.handle(request.data(), request.size()));
    const mark4::TuningAckPacket ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_VERTICAL_KP);
    REQUIRE(ack.status == mark4::TUNING_ACK_OK);
    REQUIRE(ack.value == 0.42f);

    const auto unknown = makeGet(1234U);
    REQUIRE(service.handle(unknown.data(), unknown.size()));
    REQUIRE(decodeAck(sender.sent()[1]).status == mark4::TUNING_ACK_UNKNOWN_ID);
}

TEST_CASE("a tuning list unrolls one description per pump, in table order")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    const auto request = makeList(0U);
    REQUIRE(service.handle(request.data(), request.size()));
    // The request itself emits nothing: the answer is paced by pump().
    REQUIRE(sender.sent().empty());

    // One extra pump past the end must add nothing.
    for (std::size_t i = 0U; i <= mark4::FlightCore::ParamCount(); ++i)
    {
        service.pump();
    }
    REQUIRE(sender.sent().size() == mark4::FlightCore::ParamCount());

    for (std::size_t i = 0U; i < mark4::FlightCore::ParamCount(); ++i)
    {
        const mark4::TuningInfoPacket info = decodeInfo(sender.sent()[i]);
        INFO("entry " << i);
        REQUIRE(info.index == i);
        REQUIRE(info.count == mark4::FlightCore::ParamCount());

        // Everything the description carries must match the registry entry
        // it describes: the ground side reads this instead of a table of its
        // own, so a mismatch here is a silently wrong ground station.
        const mark4::TuningParam *param = core.paramInfo(i);
        REQUIRE(param != nullptr);
        REQUIRE(info.id == param->id);
        REQUIRE(info.value == param->value);
        REQUIRE(info.minValue == param->minValue);
        REQUIRE(info.maxValue == param->maxValue);
        REQUIRE(((info.flags & mark4::TUNING_FLAG_ARMED_CHANGE) != 0U) == param->armedChange);

        // Names are zero-padded and a full-length one carries no terminator:
        // reading one must never run past the field.
        const std::string name = nameOf(info.name);
        REQUIRE(!name.empty());
        REQUIRE(name.size() <= mark4::TUNING_NAME_SIZE);
    }

    // Pumping again with nothing pending stays silent.
    service.pump();
    REQUIRE(sender.sent().size() == mark4::FlightCore::ParamCount());
}

TEST_CASE("a tuning list starting past the end answers nothing")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    const auto request = makeList(static_cast<std::uint16_t>(mark4::FlightCore::ParamCount()));
    REQUIRE(service.handle(request.data(), request.size()));
    for (std::size_t i = 0U; i < 5U; ++i)
    {
        service.pump();
    }
    REQUIRE(sender.sent().empty());
}

TEST_CASE("a new tuning list restarts the walk mid-stream")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    const auto fromStart = makeList(0U);
    REQUIRE(service.handle(fromStart.data(), fromStart.size()));
    service.pump();
    service.pump();
    REQUIRE(sender.sent().size() == 2U);
    REQUIRE(decodeInfo(sender.sent()[1]).index == 1U);

    // A second request repositions the cursor: this is how a ground station
    // asks again for the entries a lost frame took with it.
    sender.clear();
    const auto fromMiddle = makeList(3U);
    REQUIRE(service.handle(fromMiddle.data(), fromMiddle.size()));
    service.pump();
    REQUIRE(sender.sent().size() == 1U);
    REQUIRE(decodeInfo(sender.sent()[0]).index == 3U);
}

TEST_CASE("a packet that is not a tuning request is left to its owner")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    mark4::RebootCommandPacket reboot{};
    reboot.version = mark4::PROTOCOL_VERSION;
    reboot.type = static_cast<std::uint8_t>(mark4::PacketType::REBOOT_COMMAND);
    reboot.magic = mark4::BOARD_REBOOT_MAGIC;
    std::array<std::uint8_t, sizeof(reboot)> wire{};
    std::memcpy(wire.data(), &reboot, sizeof(reboot));
    REQUIRE(!service.handle(wire.data(), wire.size()));

    // Right type, wrong size, and a stale version byte: neither is ours.
    auto truncated = makeSet(mark4::TUNING_ID_AHRS_KP, 1.0f);
    truncated.pop_back();
    REQUIRE(!service.handle(truncated.data(), truncated.size()));
    auto stale = makeSet(mark4::TUNING_ID_AHRS_KP, 1.0f);
    stale[0] = static_cast<std::uint8_t>(mark4::PROTOCOL_VERSION - 1U);
    REQUIRE(!service.handle(stale.data(), stale.size()));

    REQUIRE(service.requestCount() == 0U);
    REQUIRE(sender.sent().empty());
}

TEST_CASE("the rc tracker and the tuning service share one command receiver")
{
    // This is exactly what a composition does: the tracker eats the RC
    // packets and hands back everything else, and the service claims the
    // tuning ones out of what comes back.
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    mark4::TuningService service(core, sender);

    mark4::RcCommandPacket rc{};
    rc.version = mark4::PROTOCOL_VERSION;
    rc.type = static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND);
    rc.mode = mark4::RC_MODE_ALTITUDE_AUTO;
    rc.throttle = 0.5f;
    std::vector<std::uint8_t> rcWire(sizeof(rc));
    std::memcpy(rcWire.data(), &rc, sizeof(rc));

    receiver.push(rcWire);
    receiver.push(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.6f));

    std::array<std::uint8_t, 64U> buffer{};
    std::size_t handled = 0U;
    for (;;)
    {
        const std::size_t size = tracker.pump(buffer.data(), buffer.size(), 1000U);
        if (size == 0U)
        {
            break;
        }
        if (service.handle(buffer.data(), size))
        {
            ++handled;
        }
    }

    REQUIRE(tracker.rcPacketCount() == 1U);
    REQUIRE(handled == 1U);
    REQUIRE(sender.sent().size() == 1U);
    REQUIRE(decodeAck(sender.sent()[0]).status == mark4::TUNING_ACK_OK);

    mark4::SensorFrame frame;
    frame.timestampUs = 1000U;
    tracker.graft(frame);
    REQUIRE(frame.rc.mode == mark4::PilotMode::ALTITUDE_AUTO);
}
