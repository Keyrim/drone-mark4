/// @file
/// @brief JSON codec: the shape of what the hub publishes, and the exact wire
///        bytes it builds out of what a client asks for.

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#include "hub/json_codec.hpp"
#include "hub/packed_field.hpp"

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
        packet.baroAltitudeM = 11.75f;
        return packet;
    }

    /// @brief Decodes the message the codec produced.
    /// @param text message text
    /// @return the parsed object
    nlohmann::json parsed(const std::string &text)
    {
        return nlohmann::json::parse(text);
    }

    /// Number of byte offsets a relocated packet is tried at: eight covers
    /// every residue, so each field of the struct is misaligned by at least
    /// one of them.
    constexpr std::size_t OFFSET_COUNT = 8U;

    /// @brief Copies a packed wire struct a few bytes further along.
    ///        A packet arrives as bytes, so nothing ever guarantees its
    ///        fields land aligned; this reproduces that on demand.
    /// @tparam T wire struct type
    /// @param packet packet to relocate
    /// @param storage buffer the copy lives in, resized here
    /// @param offset how far into the buffer the copy starts
    /// @return a reference to the copy
    template <typename T>
    const T &atOffset(const T &packet, std::vector<std::uint8_t> &storage, std::size_t offset)
    {
        storage.assign(sizeof(T) + OFFSET_COUNT, 0U);
        std::memcpy(&storage[offset], &packet, sizeof(T));
        return *reinterpret_cast<const T *>(&storage[offset]);
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
    CHECK(message["baroAltitudeM"] == 11.75);
    CHECK(message.size() == 17U);
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

TEST_CASE("a packet whose fields are misaligned encodes like any other")
{
    // The wire structs are packed to the byte: a sequence number or a float
    // array of theirs sits wherever the layout puts it. Encoding a packet
    // shifted by every byte offset in turn fails the sanitizer build the
    // moment something binds a reference to one of those fields.
    std::vector<std::uint8_t> storage;

    const mark4::TelemetryPacket telemetry = asymmetricTelemetry();
    const std::string telemetryText = mark4::telemetryToJson(telemetry);

    mark4::SimRawPacket simRaw{};
    simRaw.version = mark4::PROTOCOL_VERSION;
    simRaw.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_RAW);
    simRaw.sequence = 77U;
    simRaw.timestampUs = 424'242U;
    simRaw.attitudeQuat = {1.0f, 0.5f, 0.25f, 0.125f};
    simRaw.positionM = {-1.5f, 2.5f, 3.5f};
    simRaw.velocityMps = {4.5f, -5.5f, 6.5f};
    const std::string simRawText = mark4::simRawToJson(simRaw);

    mark4::TuningAckPacket ack{};
    ack.version = mark4::PROTOCOL_VERSION;
    ack.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_ACK);
    ack.id = 101U;
    ack.value = 0.028f;
    ack.status = mark4::TUNING_ACK_OK;
    const std::string ackText = mark4::tuningAckToJson(ack, mark4::StreamSource::DRONE_SIM);

    mark4::TuningInfoPacket info{};
    info.version = mark4::PROTOCOL_VERSION;
    info.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_INFO);
    info.index = 3U;
    info.count = 12U;
    info.id = 401U;
    info.name = {'a', 'h', 'r', 's', '_', 'k', 'p', '\0'};
    info.value = 2.0f;
    info.minValue = 0.5f;
    info.maxValue = 8.0f;
    const std::string infoText = mark4::tuningInfoToJson(info, mark4::StreamSource::DRONE_SIM);

    std::vector<std::uint8_t> ackStorage;
    std::vector<std::uint8_t> infoStorage;
    for (std::size_t offset = 0U; offset < OFFSET_COUNT; ++offset)
    {
        CHECK(mark4::telemetryToJson(atOffset(telemetry, storage, offset)) == telemetryText);
        CHECK(mark4::simRawToJson(atOffset(simRaw, storage, offset)) == simRawText);
        CHECK(mark4::tuningAckToJson(atOffset(ack, ackStorage, offset),
                                     mark4::StreamSource::DRONE_SIM) == ackText);
        CHECK(mark4::tuningInfoToJson(atOffset(info, infoStorage, offset),
                                      mark4::StreamSource::DRONE_SIM) == infoText);
    }
}

TEST_CASE("discovery json describes every live process")
{
    std::vector<mark4::DiscoveredProcess> processes;
    processes.push_back({mark4::StreamSource::DRONE_SIM, 7U, 47801U, 47804U, 1'000'000U, false});
    processes.push_back({mark4::StreamSource::FIRMWARE, 0U, 0U, 0U, 1'500'000U, true});

    std::vector<mark4::DiscoveredBridge> bridges;
    bridges.push_back({"192.168.1.31", 47830U, "c19f6c", 1'800'000U});

    const nlohmann::json message = parsed(mark4::discoveryToJson(processes, bridges, 2'000'000U));
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
    REQUIRE(message["bridges"].size() == 1U);
    CHECK(message["bridges"][0]["address"] == "192.168.1.31");
    CHECK(message["bridges"][0]["port"] == 47830U);
    CHECK(message["bridges"][0]["name"] == "c19f6c");
    // The page opens a link with it as it stands, without knowing the shape.
    CHECK(message["bridges"][0]["device"] == "udp:192.168.1.31:47830");
    CHECK(message["bridges"][0]["ageMs"] == 200U);
}

TEST_CASE("status json carries the counters")
{
    mark4::HubStatus status;
    status.serialOpen = true;
    status.serialLink = "udp:192.168.1.31:47830";
    status.badFrames = 40U;
    status.rejectedAnnounces = 50U;
    status.clients = 3U;
    status.rcClients = 2U;
    status.connectionVia = "bridge";
    status.connectionId = "c19f6c";
    status.connectionKind = mark4::StreamSource::FIRMWARE;
    status.connectionLive = true;

    mark4::LinkHealth link;
    link.stream = mark4::StreamKind::SIM_RAW;
    link.sourceId = 4U;
    link.sourceName = "sim_plant";
    link.received = 9U;
    link.lost = 1U;
    link.duplicates = 2U;
    link.resyncs = 3U;
    link.lastSequence = 123U;
    status.links.push_back(link);

    const nlohmann::json message = parsed(mark4::statusToJson(status));
    CHECK(message["type"] == "status");
    CHECK(message["serialOpen"] == true);
    CHECK(message["serialLink"] == "udp:192.168.1.31:47830");
    CHECK(message["counts"]["badFrames"] == 40U);
    CHECK(message["counts"]["rejectedAnnounces"] == 50U);
    CHECK(message["clients"] == 3U);
    CHECK(message["rcClients"] == 2U);
    CHECK(message["connection"]["via"] == "bridge");
    CHECK(message["connection"]["id"] == "c19f6c");
    CHECK(message["connection"]["kind"] == 1U);
    CHECK(message["connection"]["kindName"] == "firmware");
    CHECK(message["connection"]["live"] == true);

    // No connection reads as the explicit "none", never as an absent field
    mark4::HubStatus idle;
    const nlohmann::json rest = parsed(mark4::statusToJson(idle));
    CHECK(rest["connection"]["via"] == "none");
    CHECK(rest["connection"]["live"] == false);

    REQUIRE(message["links"].size() == 1U);
    const nlohmann::json &entry = message["links"][0];
    CHECK(entry["stream"] == "simRaw");
    CHECK(entry["sourceId"] == 4U);
    CHECK(entry["sourceName"] == "sim_plant");
    CHECK(entry["received"] == 9U);
    CHECK(entry["lost"] == 1U);
    CHECK(entry["duplicates"] == 2U);
    CHECK(entry["resyncs"] == 3U);
    CHECK(entry["lossRate"] == 0.1);
    CHECK(entry["lastSequence"] == 123U);
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

TEST_CASE("a scenario message becomes the exact sim scenario wire packet")
{
    const auto decoded = mark4::parseClientMessage(
        R"({"type":"simScenario","id":3,"scenario":"handThrow","sequence":7,)"
        R"("seed":81985529216486895,"throwDelayUs":2000000,"hashWindowUs":16000000,)"
        R"("velocityMps":[1.0,2.0,3.0],"angularVelocityRadS":[4.0,5.0,6.0],)"
        R"("heldSeconds":1.5,"heldTiltRad":0.25,"heldAzimuthRad":0.5,"swingSeconds":0.375})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.type == mark4::ClientMessageType::SIM_SCENARIO);
    CHECK(message.id == 3);
    // A scenario goes to the flight process driving a plant: the simulator
    // unless the message says otherwise.
    CHECK(message.target == mark4::StreamSource::DRONE_SIM);

    const auto bytes = mark4::wireBytes(message.simScenario);
    REQUIRE(bytes.size() == mark4::SIM_SCENARIO_PACKET_SIZE);
    CHECK(bytes[0] == mark4::PROTOCOL_VERSION);
    CHECK(bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::SIM_SCENARIO));

    // Read back out of the bytes: the block layout is the contract, and the
    // plant reads it at this offset inside the lockstep reply too.
    mark4::SimScenario block{};
    std::memcpy(&block, bytes.data() + offsetof(mark4::SimScenarioPacket, scenario), sizeof(block));
    CHECK(block.sequence == 7U);
    CHECK(block.scenario == mark4::SIM_SCENARIO_HAND_THROW);
    CHECK(block.seed == 0x0123456789ABCDEFULL);
    CHECK(block.throwDelayUs == 2000000U);
    CHECK(block.hashWindowUs == 16000000U);
    CHECK(mark4::readPackedField(&block.velocityMps) == std::array<float, 3>{1.0f, 2.0f, 3.0f});
    CHECK(mark4::readPackedField(&block.angularVelocityRadS) ==
          std::array<float, 3>{4.0f, 5.0f, 6.0f});
    CHECK(block.heldSeconds == 1.5f);
    CHECK(block.heldTiltRad == 0.25f);
    CHECK(block.heldAzimuthRad == 0.5f);
    CHECK(block.swingSeconds == 0.375f);
}

TEST_CASE("a reset scenario message needs nothing but its name")
{
    const auto decoded = mark4::parseClientMessage(R"({"type":"simScenario","scenario":"reset"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.simScenario.scenario.scenario == mark4::SIM_SCENARIO_RESET);
    // Left at 0, so the hub stamps its own number before sending.
    CHECK(message.simScenario.scenario.sequence == 0U);

    const auto thrown = mark4::parseClientMessage(R"({"type":"simScenario","scenario":"throw"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(thrown));
    CHECK(std::get<mark4::ClientMessage>(thrown).simScenario.scenario.scenario ==
          mark4::SIM_SCENARIO_THROW);
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

TEST_CASE("a correlation id past the int range is refused, never wrapped")
{
    // A wrapped id would come back in the ack and match nothing: the page
    // would see a timeout instead of an answer
    const auto tooBig =
        mark4::parseClientMessage(R"({"type":"reboot","target":"firmware","id":4294901760})");
    CHECK(std::holds_alternative<std::string>(tooBig));
    const auto negative =
        mark4::parseClientMessage(R"({"type":"reboot","target":"firmware","id":-2})");
    CHECK(std::holds_alternative<std::string>(negative));
}

TEST_CASE("a connect message names one drone by its route")
{
    const auto udp =
        mark4::parseClientMessage(R"({"type":"connect","via":"udp","target":"drone_sim"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(udp));
    CHECK(std::get<mark4::ClientMessage>(udp).type == mark4::ClientMessageType::CONNECT);
    CHECK(std::get<mark4::ClientMessage>(udp).connectVia == "udp");
    CHECK(std::get<mark4::ClientMessage>(udp).target == mark4::StreamSource::DRONE_SIM);

    const auto bridge =
        mark4::parseClientMessage(R"({"type":"connect","via":"bridge","name":"c19f6c"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(bridge));
    CHECK(std::get<mark4::ClientMessage>(bridge).connectVia == "bridge");
    CHECK(std::get<mark4::ClientMessage>(bridge).connectPeer == "c19f6c");

    const auto disconnect = mark4::parseClientMessage(R"({"type":"disconnect"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(disconnect));
    CHECK(std::get<mark4::ClientMessage>(disconnect).type == mark4::ClientMessageType::DISCONNECT);

    // Each route requires its identity: no name and no target means there
    // is nothing to connect to
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"udp"})")));
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"bridge"})")));
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"carrier-pigeon"})")));
}

TEST_CASE("a tuning set message becomes the exact tuning set wire packet")
{
    const auto decoded = mark4::parseClientMessage(
        R"({"type":"tuningSet","id":7,"target":"drone_sim","paramId":101,"value":0.028})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(decoded));
    const auto &message = std::get<mark4::ClientMessage>(decoded);
    CHECK(message.type == mark4::ClientMessageType::TUNING_SET);
    CHECK(message.id == 7);
    CHECK(message.target == mark4::StreamSource::DRONE_SIM);

    const auto bytes = mark4::wireBytes(message.tuningSet);
    REQUIRE(bytes.size() == mark4::TUNING_SET_PACKET_SIZE);
    CHECK(bytes[0] == mark4::PROTOCOL_VERSION);
    CHECK(bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::TUNING_SET));
    std::uint16_t paramId = 0U;
    std::memcpy(&paramId, &bytes[2], sizeof(paramId));
    CHECK(paramId == 101U);
    float value = 0.0f;
    std::memcpy(&value, &bytes[4], sizeof(value));
    CHECK(value == 0.028f);
}

TEST_CASE("a tuning get and a tuning list become their exact wire packets")
{
    const auto get = mark4::parseClientMessage(
        R"({"type":"tuningGet","id":8,"target":"firmware","paramId":303})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(get));
    const auto &getMessage = std::get<mark4::ClientMessage>(get);
    CHECK(getMessage.type == mark4::ClientMessageType::TUNING_GET);
    const auto getBytes = mark4::wireBytes(getMessage.tuningGet);
    REQUIRE(getBytes.size() == mark4::TUNING_GET_PACKET_SIZE);
    CHECK(getBytes[1] == static_cast<std::uint8_t>(mark4::PacketType::TUNING_GET));
    std::uint16_t paramId = 0U;
    std::memcpy(&paramId, &getBytes[2], sizeof(paramId));
    CHECK(paramId == 303U);

    // startIndex is optional and defaults to the top of the table.
    const auto list =
        mark4::parseClientMessage(R"({"type":"tuningList","id":9,"target":"drone_sim"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(list));
    const auto &listMessage = std::get<mark4::ClientMessage>(list);
    CHECK(listMessage.type == mark4::ClientMessageType::TUNING_LIST);
    const auto listBytes = mark4::wireBytes(listMessage.tuningList);
    REQUIRE(listBytes.size() == mark4::TUNING_LIST_PACKET_SIZE);
    std::uint16_t startIndex = 1U;
    std::memcpy(&startIndex, &listBytes[2], sizeof(startIndex));
    CHECK(startIndex == 0U);

    const auto paged =
        mark4::parseClientMessage(R"({"type":"tuningList","target":"drone_sim","startIndex":4})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(paged));
    const auto pagedBytes = mark4::wireBytes(std::get<mark4::ClientMessage>(paged).tuningList);
    std::memcpy(&startIndex, &pagedBytes[2], sizeof(startIndex));
    CHECK(startIndex == 4U);
}

TEST_CASE("tuning ack json names the status it carries")
{
    mark4::TuningAckPacket packet{};
    packet.version = mark4::PROTOCOL_VERSION;
    packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_ACK);
    packet.id = 101U;
    packet.value = 0.028f;
    packet.status = mark4::TUNING_ACK_OK;

    const nlohmann::json message =
        parsed(mark4::tuningAckToJson(packet, mark4::StreamSource::DRONE_SIM));
    CHECK(message["type"] == "tuningAck");
    CHECK(message["source"] == "drone_sim");
    // The parameter id key is never "id": that one is the correlation id.
    CHECK(message["paramId"] == 101U);
    CHECK(message["value"] == 0.028f);
    CHECK(message["status"] == 0U);
    CHECK(message["statusName"] == "ok");
    CHECK(message.size() == 6U);

    const char *names[] = {"ok", "unknownId", "outOfBounds", "lockedWhileArmed", "unknown"};
    for (std::uint8_t status = 0U; status < 5U; ++status)
    {
        packet.status = status;
        const nlohmann::json named =
            parsed(mark4::tuningAckToJson(packet, mark4::StreamSource::FIRMWARE));
        INFO("status " << static_cast<unsigned>(status));
        CHECK(named["statusName"] == names[status]);
        CHECK(named["source"] == "firmware");
    }
}

TEST_CASE("tuning info json reads a name that fills the whole wire field")
{
    mark4::TuningInfoPacket packet{};
    packet.version = mark4::PROTOCOL_VERSION;
    packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_INFO);
    packet.index = 3U;
    packet.count = 12U;
    packet.id = 401U;
    packet.name = {'a', 'h', 'r', 's', '_', 'k', 'p', '\0'};
    packet.value = 2.0f;
    packet.minValue = 0.5f;
    packet.maxValue = 8.0f;
    packet.flags = 0U;

    const nlohmann::json message =
        parsed(mark4::tuningInfoToJson(packet, mark4::StreamSource::DRONE_SIM));
    CHECK(message["type"] == "tuningInfo");
    CHECK(message["source"] == "drone_sim");
    CHECK(message["index"] == 3U);
    CHECK(message["count"] == 12U);
    CHECK(message["paramId"] == 401U);
    CHECK(message["name"] == "ahrs_kp");
    CHECK(message["value"] == 2.0);
    CHECK(message["minValue"] == 0.5);
    CHECK(message["maxValue"] == 8.0);
    CHECK(message["armedChange"] == false);
    CHECK(message.size() == 10U);

    // A name filling the field carries no terminator: reading it must stop
    // at the field boundary rather than run off the end of the struct.
    for (char &character : packet.name)
    {
        character = 'x';
    }
    packet.flags = mark4::TUNING_FLAG_ARMED_CHANGE;
    const nlohmann::json full =
        parsed(mark4::tuningInfoToJson(packet, mark4::StreamSource::DRONE_SIM));
    CHECK(full["name"] == std::string(mark4::TUNING_NAME_SIZE, 'x'));
    CHECK(full["armedChange"] == true);
}

TEST_CASE("profile messages decode and render")
{
    const auto list = mark4::parseClientMessage(R"({"type":"profileList","id":10})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(list));
    CHECK(std::get<mark4::ClientMessage>(list).type == mark4::ClientMessageType::PROFILE_LIST);

    const auto save = mark4::parseClientMessage(
        R"({"type":"profileSave","id":11,"name":"bench","values":{"101":0.028,"303":0.55}})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(save));
    const auto &saveMessage = std::get<mark4::ClientMessage>(save);
    CHECK(saveMessage.type == mark4::ClientMessageType::PROFILE_SAVE);
    CHECK(saveMessage.profileName == "bench");
    REQUIRE(saveMessage.profileValues.size() == 2U);
    CHECK(saveMessage.profileValues.at(101U) == 0.028f);
    CHECK(saveMessage.profileValues.at(303U) == 0.55f);

    const auto load = mark4::parseClientMessage(R"({"type":"profileLoad","id":12,"name":"bench"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(load));
    CHECK(std::get<mark4::ClientMessage>(load).profileName == "bench");

    const auto push = mark4::parseClientMessage(
        R"({"type":"profilePush","id":13,"name":"bench","target":"drone_sim"})");
    REQUIRE(std::holds_alternative<mark4::ClientMessage>(push));
    const auto &pushMessage = std::get<mark4::ClientMessage>(push);
    CHECK(pushMessage.type == mark4::ClientMessageType::PROFILE_PUSH);
    CHECK(pushMessage.target == mark4::StreamSource::DRONE_SIM);

    const nlohmann::json names = parsed(mark4::profileNamesToJson({"bench", "field-2"}));
    CHECK(names["type"] == "profiles");
    CHECK(names["names"] == nlohmann::json({"bench", "field-2"}));

    mark4::TuningValues values;
    values[101U] = 0.028f;
    const nlohmann::json profile = parsed(mark4::profileToJson("bench", values));
    CHECK(profile["type"] == "profile");
    CHECK(profile["name"] == "bench");
    CHECK(profile["values"]["101"] == 0.028f);
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
        R"({"type":"simScenario"})",
        R"({"type":"simScenario","scenario":"levitate"})",
        R"({"type":"simScenario","scenario":"throw","velocityMps":[1.0,2.0]})",
        R"({"type":"simScenario","scenario":"throw","velocityMps":"fast"})",
        R"({"type":"simScenario","scenario":"throw","seed":-1})",
        R"({"type":"simScenario","scenario":"throw","sequence":300})",
        R"({"type":"simScenario","scenario":"throw","target":"ghost"})",
        R"({"type":"reboot"})",
        R"({"type":"tuningSet","target":"drone_sim","value":1.0})",
        R"({"type":"tuningSet","target":"drone_sim","paramId":101})",
        R"({"type":"tuningSet","target":"drone_sim","paramId":101,"value":"fast"})",
        R"({"type":"tuningSet","target":"drone_sim","paramId":-1,"value":1.0})",
        R"({"type":"tuningSet","target":"drone_sim","paramId":70000,"value":1.0})",
        R"({"type":"tuningSet","paramId":101,"value":1.0})",
        R"({"type":"tuningGet","target":"drone_sim"})",
        R"({"type":"tuningGet","target":"ghost","paramId":101})",
        R"({"type":"tuningList"})",
        R"({"type":"tuningList","target":"drone_sim","startIndex":"first"})",
        R"({"type":"profileSave","values":{}})",
        R"({"type":"profileSave","name":"../escape","values":{}})",
        R"({"type":"profileSave","name":"with space","values":{}})",
        R"({"type":"profileSave","name":"bench"})",
        R"({"type":"profileSave","name":"bench","values":[1,2]})",
        R"({"type":"profileSave","name":"bench","values":{"kp":0.1}})",
        R"({"type":"profileSave","name":"bench","values":{"101":"fast"}})",
        R"({"type":"profileSave","name":"bench","values":{"70000":0.1}})",
        R"({"type":"profileLoad"})",
        R"({"type":"profilePush","name":"bench"})",
        R"({"type":"profilePush","name":"bench","target":"ghost"})",
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
