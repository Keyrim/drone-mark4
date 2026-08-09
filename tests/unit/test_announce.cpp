#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "platform/telemetry_sender.hpp"
#include "platform_common/announce_publisher.hpp"
#include "protocol/announce.hpp"
#include "protocol/header.hpp"
#include "protocol/ports.hpp"

namespace
{
    constexpr std::uint64_t T0_US = 5000000U;

    /// Telemetry sender keeping every datagram handed to it, so a test can
    /// check the exact bytes that went out. Allocates freely: this is a
    /// test, not flight code.
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

      private:
        std::vector<std::vector<std::uint8_t>> m_sent; ///< captured datagrams
    };

    /// @return the bytes an announce for this composition must carry
    std::vector<std::uint8_t> expectedAnnounce()
    {
        mark4::AnnouncePacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::ANNOUNCE);
        packet.kind = static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM);
        packet.sessionId = 0U;
        packet.telemetryPort = mark4::TELEMETRY_PORT;
        packet.commandPort = mark4::RC_COMMAND_PORT;

        std::vector<std::uint8_t> wire(sizeof(packet));
        std::memcpy(wire.data(), &packet, sizeof(packet));
        return wire;
    }
} // namespace

TEST_CASE("the first announce goes out immediately, with the expected bytes")
{
    FakeTelemetrySender sender;
    mark4::AnnouncePublisher publisher(
        sender, mark4::StreamSource::DRONE_SIM, mark4::TELEMETRY_PORT, mark4::RC_COMMAND_PORT);

    publisher.publish(T0_US);

    REQUIRE(sender.sent().size() == 1U);
    REQUIRE(publisher.announceCount() == 1U);

    const std::vector<std::uint8_t> &wire = sender.sent().front();
    REQUIRE(wire.size() == mark4::ANNOUNCE_PACKET_SIZE);
    REQUIRE(wire == expectedAnnounce());

    // Spelled out field by field too: these offsets are the wire contract.
    REQUIRE(wire[0] == mark4::PROTOCOL_VERSION);
    REQUIRE(wire[1] == static_cast<std::uint8_t>(mark4::PacketType::ANNOUNCE));
    REQUIRE(wire[2] == static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM));

    std::uint32_t sessionId = 1U;
    std::memcpy(
        &sessionId, wire.data() + offsetof(mark4::AnnouncePacket, sessionId), sizeof(sessionId));
    REQUIRE(sessionId == 0U);

    std::uint16_t telemetryPort = 0U;
    std::memcpy(&telemetryPort,
                wire.data() + offsetof(mark4::AnnouncePacket, telemetryPort),
                sizeof(telemetryPort));
    REQUIRE(telemetryPort == mark4::TELEMETRY_PORT);

    std::uint16_t commandPort = 0U;
    std::memcpy(&commandPort,
                wire.data() + offsetof(mark4::AnnouncePacket, commandPort),
                sizeof(commandPort));
    REQUIRE(commandPort == mark4::RC_COMMAND_PORT);
}

TEST_CASE("announces are broadcast once per period")
{
    FakeTelemetrySender sender;
    mark4::AnnouncePublisher publisher(
        sender, mark4::StreamSource::DRONE_SIM, mark4::TELEMETRY_PORT, mark4::RC_COMMAND_PORT);

    publisher.publish(T0_US);
    REQUIRE(sender.sent().size() == 1U);

    // One microsecond short of the period: nothing goes out.
    publisher.publish(T0_US + mark4::AnnouncePublisher::PERIOD_US - 1U);
    REQUIRE(sender.sent().size() == 1U);

    // Exactly one period later: the next announce goes out.
    publisher.publish(T0_US + mark4::AnnouncePublisher::PERIOD_US);
    REQUIRE(sender.sent().size() == 2U);
    REQUIRE(publisher.announceCount() == 2U);
    REQUIRE(sender.sent().back() == expectedAnnounce());
}
