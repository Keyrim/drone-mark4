/// @file
/// @brief JSON codec: the shape of what the hub publishes, and the exact wire
///        bytes it builds out of what a client asks for.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

#include "hub/json_codec.hpp"

namespace
{
    /// @brief Builds a telemetry packet whose every field differs from every
    ///        other, so a swapped pair of keys cannot go unnoticed.
    /// @return the packet
    mark4::TelemetryPacket asymmetricTelemetry()
    {
        mark4::TelemetryPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TELEMETRY);
        packet.sourceId = static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM);
        packet.sequence = 1234U;
        packet.timestampUs = 9'876'543'210U;
        packet.gyroRadS = {0.25f, -0.5f, 0.75f};
        packet.attitudeQuat = {0.5f, -0.25f, 0.125f, -0.0625f};
        packet.gyroBiasRadS = {1.5f, -2.5f, 3.5f};
        packet.motor = {0.125f, 0.25f, 0.375f, 0.5f};
        packet.altitudeM = 12.5f;
        packet.verticalVelocityMps = -3.25f;
        packet.throwState = 2U;
        packet.throwCount = 5U;
        packet.releaseVelocityMps = 6.75f;
        packet.apexTimestampUs = 1'234'567'890U;
        packet.apexAltitudeM = 8.5f;
        packet.flightPhase = 4U;
        return packet;
    }

    /// @brief Decodes the message the codec produced.
    /// @param text message text
    /// @return the parsed object
    nlohmann::json parsed(const std::string &text)
    {
        return nlohmann::json::parse(text);
    }
} // namespace

TEST_CASE("telemetry json carries every field of the packet")
{
    const mark4::TelemetryPacket packet = asymmetricTelemetry();
    const nlohmann::json message = parsed(mark4::telemetryToJson(packet));

    CHECK(message["type"] == "telemetry");
    CHECK(message["sourceId"] == 2U);
    CHECK(message["sequence"] == 1234U);
    CHECK(message["timestampUs"] == 9'876'543'210U);
    CHECK(message["gyroRadS"] == nlohmann::json({0.25, -0.5, 0.75}));
    CHECK(message["attitudeQuat"] == nlohmann::json({0.5, -0.25, 0.125, -0.0625}));
    CHECK(message["gyroBiasRadS"] == nlohmann::json({1.5, -2.5, 3.5}));
    CHECK(message["motor"] == nlohmann::json({0.125, 0.25, 0.375, 0.5}));
    CHECK(message["altitudeM"] == 12.5);
    CHECK(message["verticalVelocityMps"] == -3.25);
    CHECK(message["throwState"] == 2U);
    CHECK(message["throwCount"] == 5U);
    CHECK(message["releaseVelocityMps"] == 6.75);
    CHECK(message["apexTimestampUs"] == 1'234'567'890U);
    CHECK(message["apexAltitudeM"] == 8.5);
    CHECK(message["flightPhase"] == 4U);
    CHECK(message.size() == 16U);
}

TEST_CASE("sim raw json carries every field of the packet")
{
    mark4::SimRawPacket packet{};
    packet.version = mark4::PROTOCOL_VERSION;
    packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_RAW);
    packet.sourceId = static_cast<std::uint8_t>(mark4::StreamSource::SIM_PLANT);
    packet.sequence = 77U;
    packet.timestampUs = 424'242U;
    packet.attitudeQuat = {1.0f, 0.5f, 0.25f, 0.125f};
    packet.positionM = {-1.5f, 2.5f, 3.5f};
    packet.velocityMps = {4.5f, -5.5f, 6.5f};

    const nlohmann::json message = parsed(mark4::simRawToJson(packet));
    CHECK(message["type"] == "simRaw");
    CHECK(message["sourceId"] == 4U);
    CHECK(message["sequence"] == 77U);
    CHECK(message["timestampUs"] == 424'242U);
    CHECK(message["attitudeQuat"] == nlohmann::json({1.0, 0.5, 0.25, 0.125}));
    CHECK(message["positionM"] == nlohmann::json({-1.5, 2.5, 3.5}));
    CHECK(message["velocityMps"] == nlohmann::json({4.5, -5.5, 6.5}));
    CHECK(message.size() == 7U);
}

TEST_CASE("discovery json describes every live process")
{
    std::vector<mark4::DiscoveredProcess> processes;
    processes.push_back({mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U, 1'000'000U, false});
    processes.push_back({mark4::StreamSource::FIRMWARE, 0U, 0U, 0U, 1'500'000U, true});

    const nlohmann::json message = parsed(mark4::discoveryToJson(processes, 2'000'000U));
    CHECK(message["type"] == "discovery");
    REQUIRE(message["processes"].size() == 2U);
    CHECK(message["processes"][0]["kind"] == 2U);
    CHECK(message["processes"][0]["kindName"] == "drone_sim");
    CHECK(message["processes"][0]["sessionId"] == 7U);
    CHECK(message["processes"][0]["telemetryPort"] == 47801U);
    CHECK(message["processes"][0]["commandPort"] == 47804U);
    CHECK(message["processes"][0]["viaSerial"] == false);
    CHECK(message["processes"][0]["ageMs"] == 1000U);
    CHECK(message["processes"][1]["kindName"] == "firmware");
    CHECK(message["processes"][1]["viaSerial"] == true);
    CHECK(message["processes"][1]["ageMs"] == 500U);
}

TEST_CASE("status json carries the counters")
{
    mark4::HubStatus status;
    status.recording = true;
    status.serialOpen = false;
    status.telemetryRows = 10U;
    status.simRawRows = 20U;
    status.blackboxRecords = 30U;
    status.badFrames = 40U;
    status.rejectedAnnounces = 50U;
    status.clients = 3U;

    const nlohmann::json message = parsed(mark4::statusToJson(status));
    CHECK(message["type"] == "status");
    CHECK(message["recording"] == true);
    CHECK(message["serialOpen"] == false);
    CHECK(message["counts"]["telemetryRows"] == 10U);
    CHECK(message["counts"]["simRawRows"] == 20U);
    CHECK(message["counts"]["blackboxRecords"] == 30U);
    CHECK(message["counts"]["badFrames"] == 40U);
    CHECK(message["counts"]["rejectedAnnounces"] == 50U);
    CHECK(message["clients"] == 3U);
}

TEST_CASE("an ack answers one request by its correlation id")
{
    const nlohmann::json ok = parsed(mark4::ackToJson(7, true, ""));
    CHECK(ok["type"] == "ack");
    CHECK(ok["id"] == 7);
    CHECK(ok["ok"] == true);
    CHECK(ok["error"].get<std::string>().empty());

    const nlohmann::json nack = parsed(mark4::ackToJson(8, false, "no serial link"));
    CHECK(nack["ok"] == false);
    CHECK(nack["error"] == "no serial link");
}

TEST_CASE("an rc message becomes the exact rc wire packet")
{
    const auto decoded = mark4::parseClientMessage(
        R"({"type":"rc","id":7,"target":"firmware","kill":0,"arm":1,"mode":1,"throttle":0.5})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.type == mark4::ClientMessageType::RC);
    CHECK(message.id == 7);
    CHECK(message.target == mark4::StreamSource::FIRMWARE);

    const auto bytes = mark4::wireBytes(message.rc);
    REQUIRE(bytes.size() == mark4::RC_COMMAND_PACKET_SIZE);
    CHECK(bytes[0] == mark4::PROTOCOL_VERSION);
    CHECK(bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND));
    CHECK(bytes[2] == 0U);
    CHECK(bytes[3] == 1U);
    CHECK(bytes[4] == mark4::RC_MODE_ALTITUDE_AUTO);
    float throttle = 0.0f;
    std::memcpy(&throttle, &bytes[5], sizeof(throttle));
    CHECK(throttle == 0.5f);
}

TEST_CASE("an rc message can target the simulator")
{
    const auto decoded = mark4::parseClientMessage(
        R"({"type":"rc","target":"drone_sim","kill":1,"arm":0,"mode":0,"throttle":0})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.target == mark4::StreamSource::DRONE_SIM);
    CHECK(message.id == -1);
    CHECK(message.rc.killSwitch == 1U);
}

TEST_CASE("a scenario message becomes the exact sim command wire packet")
{
    const auto decoded = mark4::parseClientMessage(
        R"({"type":"simCommand","id":3,"command":"handThrow",)"
        R"("velocityMps":[1.0,2.0,3.0],"angularVelocityRadS":[4.0,5.0,6.0],)"
        R"("heldSeconds":1.5,"heldTiltRad":0.25,"heldAzimuthRad":0.5,"swingSeconds":0.375})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.type == mark4::ClientMessageType::SIM_COMMAND);
    CHECK(message.id == 3);

    const auto bytes = mark4::wireBytes(message.simCommand);
    REQUIRE(bytes.size() == mark4::SIM_COMMAND_PACKET_SIZE);
    CHECK(bytes[0] == mark4::PROTOCOL_VERSION);
    CHECK(bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::SIM_COMMAND));
    CHECK(bytes[2] == mark4::SIM_COMMAND_HAND_THROW);
    CHECK(message.simCommand.velocityMps == std::array<float, 3>{1.0f, 2.0f, 3.0f});
    CHECK(message.simCommand.angularVelocityRadS == std::array<float, 3>{4.0f, 5.0f, 6.0f});
    CHECK(message.simCommand.heldSeconds == 1.5f);
    CHECK(message.simCommand.heldTiltRad == 0.25f);
    CHECK(message.simCommand.heldAzimuthRad == 0.5f);
    CHECK(message.simCommand.swingSeconds == 0.375f);
}

TEST_CASE("a reset scenario message needs nothing but its name")
{
    const auto decoded = mark4::parseClientMessage(R"({"type":"simCommand","command":"reset"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    CHECK(std::get<mark4::ClientMessage>(decoded).simCommand.command == mark4::SIM_COMMAND_RESET);

    const auto thrown = mark4::parseClientMessage(R"({"type":"simCommand","command":"throw"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(thrown));
    CHECK(std::get<mark4::ClientMessage>(thrown).simCommand.command == mark4::SIM_COMMAND_THROW);
}

TEST_CASE("a reboot message carries the board reboot magic")
{
    const auto decoded =
        mark4::parseClientMessage(R"({"type":"reboot","id":11,"target":"firmware"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.type == mark4::ClientMessageType::REBOOT);

    const auto bytes = mark4::wireBytes(message.reboot);
    REQUIRE(bytes.size() == mark4::REBOOT_COMMAND_PACKET_SIZE);
    CHECK(bytes[0] == mark4::PROTOCOL_VERSION);
    CHECK(bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::REBOOT_COMMAND));
    CHECK(bytes[2] == mark4::BOARD_REBOOT_MAGIC);
}

TEST_CASE("a record message toggles the csv session")
{
    const auto start = mark4::parseClientMessage(R"({"type":"record","action":"start"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(start));
    CHECK(std::get<mark4::ClientMessage>(start).type == mark4::ClientMessageType::RECORD);
    CHECK(std::get<mark4::ClientMessage>(start).recordStart);

    const auto stop = mark4::parseClientMessage(R"({"type":"record","action":"stop"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(stop));
    CHECK(!(std::get<mark4::ClientMessage>(stop).recordStart));
}

TEST_CASE("a malformed client message is refused, never thrown")
{
    const char *rejected[] = {
        "",
        "not json at all",
        "[1, 2, 3]",
        R"({"id":1})",
        R"({"type":42})",
        R"({"type":"nonsense"})",
        R"({"type":"rc","target":"firmware","throttle":"fast"})",
        R"({"type":"rc","target":"firmware","throttle":1.5})",
        R"({"type":"rc","target":"firmware","throttle":-0.1})",
        R"({"type":"rc","target":"ghost"})",
        R"({"type":"rc","target":"firmware","kill":300})",
        R"({"type":"rc","target":"firmware","mode":"auto"})",
        R"({"type":"rc","id":"seven","target":"firmware"})",
        R"({"type":"simCommand"})",
        R"({"type":"simCommand","command":"levitate"})",
        R"({"type":"simCommand","command":"throw","velocityMps":[1.0,2.0]})",
        R"({"type":"simCommand","command":"throw","velocityMps":"fast"})",
        R"({"type":"reboot"})",
        R"({"type":"record"})",
        R"({"type":"record","action":"pause"})",
    };
    for (const char *text : rejected)
    {
        const auto decoded = mark4::parseClientMessage(text);
        INFO("message: " << text);
        REQUIRE(std::holds_alternative<std::string>(decoded));
        CHECK(!(std::get<std::string>(decoded).empty()));
    }
}

TEST_CASE("a rejected message can still be answered by its id")
{
    CHECK(mark4::clientMessageId(R"({"type":"rc","id":12,"throttle":"fast"})") == 12);
    CHECK(mark4::clientMessageId(R"({"type":"rc","throttle":"fast"})") == -1);
    CHECK(mark4::clientMessageId("not json at all") == -1);
}
