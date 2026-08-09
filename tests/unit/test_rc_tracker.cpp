#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/types.hpp"
#include "platform/command_receiver.hpp"
#include "platform_common/rc_tracker.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 1000000U;
    constexpr float TEST_THROTTLE = 0.625f;

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

    /// @param kill 1 = engaged
    /// @param arm 1 = armed
    /// @param throttle normalized throttle
    /// @param version version byte, PROTOCOL_VERSION unless testing drift
    /// @return wire bytes of one RcCommandPacket
    std::vector<std::uint8_t> makeRcPacket(std::uint8_t kill,
                                           std::uint8_t arm,
                                           float throttle,
                                           std::uint8_t version = mark4::PROTOCOL_VERSION)
    {
        mark4::RcCommandPacket packet{};
        packet.version = version;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND);
        packet.killSwitch = kill;
        packet.armSwitch = arm;
        packet.mode = mark4::RC_MODE_MANUAL;
        packet.throttle = throttle;

        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }

    /// @return wire bytes of one RebootCommandPacket
    std::vector<std::uint8_t> makeRebootPacket()
    {
        mark4::RebootCommandPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::REBOOT_COMMAND);
        packet.magic = mark4::BOARD_REBOOT_MAGIC;

        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }

    /// Scratch buffer for the packets pump() hands back.
    using PumpBuffer = std::array<std::uint8_t, 64U>;
} // namespace

TEST_CASE("before any rc packet the frame reverts to the safe state")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.throttle = TEST_THROTTLE;

    REQUIRE(tracker.pump(buffer.data(), buffer.size(), frame.timestampUs) == 0U);
    REQUIRE(tracker.failsafeActive(frame.timestampUs));
    tracker.graft(frame);

    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(frame.rc.armSwitch == false);
    REQUIRE(frame.rc.throttle == 0.0f);
    REQUIRE(tracker.rcPacketCount() == 0U);
}

TEST_CASE("an rc command packet reaches the frame")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE));

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), frame.timestampUs) == 0U);
    REQUIRE(tracker.rcPacketCount() == 1U);
    REQUIRE_FALSE(tracker.failsafeActive(frame.timestampUs));
    tracker.graft(frame);

    REQUIRE(frame.rc.killSwitch == false);
    REQUIRE(frame.rc.armSwitch == true);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("silence on the rc uplink reverts to kill within the timeout")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE));
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), T0_US) == 0U);

    // The whole timeout window still holds the last known state.
    mark4::SensorFrame edge;
    edge.timestampUs = T0_US + mark4::RcTracker::RC_TIMEOUT_US;
    REQUIRE_FALSE(tracker.failsafeActive(edge.timestampUs));
    tracker.graft(edge);
    REQUIRE(edge.rc.killSwitch == false);
    REQUIRE(edge.rc.armSwitch == true);
    REQUIRE(edge.rc.throttle == TEST_THROTTLE);

    // One microsecond past it, the safe state wins.
    mark4::SensorFrame lost;
    lost.timestampUs = T0_US + mark4::RcTracker::RC_TIMEOUT_US + 1U;
    REQUIRE(tracker.failsafeActive(lost.timestampUs));
    tracker.graft(lost);
    REQUIRE(lost.rc.killSwitch == true);
    REQUIRE(lost.rc.armSwitch == false);
    REQUIRE(lost.rc.throttle == 0.0f);
}

TEST_CASE("a fresh rc packet recovers from the fail-safe")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE));
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), T0_US) == 0U);

    const std::uint64_t lateUs = T0_US + 2U * mark4::RcTracker::RC_TIMEOUT_US;
    REQUIRE(tracker.failsafeActive(lateUs));

    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE));
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), lateUs) == 0U);
    REQUIRE_FALSE(tracker.failsafeActive(lateUs));

    mark4::SensorFrame frame;
    frame.timestampUs = lateUs;
    tracker.graft(frame);
    REQUIRE(frame.rc.killSwitch == false);
    REQUIRE(frame.rc.armSwitch == true);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("non-rc packets pass through to the caller")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE));
    receiver.push(makeRebootPacket());

    // The RC packet is consumed on the way, the reboot comes out intact.
    const std::size_t size = tracker.pump(buffer.data(), buffer.size(), T0_US);
    REQUIRE(size == mark4::REBOOT_COMMAND_PACKET_SIZE);
    REQUIRE(mark4::hasHeader(buffer.data(), size, mark4::PacketType::REBOOT_COMMAND));
    REQUIRE(buffer[2] == mark4::BOARD_REBOOT_MAGIC);
    REQUIRE(tracker.rcPacketCount() == 1U);
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), T0_US) == 0U);
}

TEST_CASE("the last rc packet of a burst wins")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    receiver.push(makeRcPacket(0U, 0U, 0.25f));
    receiver.push(makeRcPacket(0U, 1U, 0.5f));
    receiver.push(makeRcPacket(1U, 0U, TEST_THROTTLE));

    REQUIRE(tracker.pump(buffer.data(), buffer.size(), T0_US) == 0U);
    REQUIRE(tracker.rcPacketCount() == 3U);

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    tracker.graft(frame);
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(frame.rc.armSwitch == false);
    REQUIRE(frame.rc.throttle == TEST_THROTTLE);
}

TEST_CASE("a wrong version is not consumed as rc")
{
    FakeCommandReceiver receiver;
    mark4::RcTracker tracker(receiver);
    PumpBuffer buffer{};

    const auto stale = static_cast<std::uint8_t>(mark4::PROTOCOL_VERSION - 1U);
    receiver.push(makeRcPacket(0U, 1U, TEST_THROTTLE, stale));

    // Right size, wrong version: handed back to the caller, state untouched.
    REQUIRE(tracker.pump(buffer.data(), buffer.size(), T0_US) == mark4::RC_COMMAND_PACKET_SIZE);
    REQUIRE(tracker.rcPacketCount() == 0U);
    REQUIRE(tracker.failsafeActive(T0_US));

    mark4::SensorFrame frame;
    frame.timestampUs = T0_US;
    tracker.graft(frame);
    REQUIRE(frame.rc.killSwitch == true);
    REQUIRE(frame.rc.throttle == 0.0f);
}
