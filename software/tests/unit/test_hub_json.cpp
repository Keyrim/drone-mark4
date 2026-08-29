/// @file
/// @brief JSON codec: the shape of what the hub publishes, and the exact wire
///        message it builds out of what a client asks for.

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>
#include <vector>

#include "hub/json_codec.hpp"

namespace
{
    /// @brief Builds a telemetry message whose every field differs from every
    ///        other, so a swapped pair of keys cannot go unnoticed.
    /// @return the message
    mark4_Telemetry asymmetricTelemetry()
    {
        mark4_Telemetry telemetry = mark4_Telemetry_init_zero;
        telemetry.timestamp_us = 9'876'543'210U;
        const float gyro[3] = {0.25f, -0.5f, 0.75f};
        const float quat[4] = {0.5f, -0.25f, 0.125f, -0.0625f};
        const float bias[3] = {1.5f, -2.5f, 3.5f};
        const float motor[4] = {0.125f, 0.25f, 0.375f, 0.5f};
        std::memcpy(telemetry.gyro_rad_s, gyro, sizeof(gyro));
        std::memcpy(telemetry.attitude_quat, quat, sizeof(quat));
        std::memcpy(telemetry.gyro_bias_rad_s, bias, sizeof(bias));
        std::memcpy(telemetry.motor, motor, sizeof(motor));
        telemetry.altitude_m = 12.5f;
        telemetry.vertical_velocity_mps = -3.25f;
        telemetry.throw_state = mark4_ThrowState_THROW_BALLISTIC;
        telemetry.throw_count = 5U;
        telemetry.release_velocity_mps = 6.75f;
        telemetry.apex_timestamp_us = 1'234'567'890U;
        telemetry.apex_altitude_m = 8.5f;
        telemetry.flight_phase = mark4_FlightPhase_PHASE_RECOVERY;
        telemetry.baro_altitude_m = 11.75f;
        return telemetry;
    }

    /// @brief Decodes the message the codec produced.
    /// @param text message text
    /// @return the parsed object
    nlohmann::json parsed(const std::string &text)
    {
        return nlohmann::json::parse(text);
    }

    /// @brief Decodes one client message, requiring it to be valid.
    /// @param text message text
    /// @return the decoded request
    mark4::ClientMessage decoded(std::string_view text)
    {
        const auto result = mark4::parseClientMessage(text);
        REQUIRE(std::holds_alternative<mark4::ClientMessage>(result));
        return std::get<mark4::ClientMessage>(result);
    }
} // namespace

TEST_CASE("telemetry json carries every field of the message")
{
    const mark4_Telemetry telemetry = asymmetricTelemetry();
    const nlohmann::json message =
        parsed(mark4::telemetryToJson(telemetry, mark4_NodeKind_DRONE_SIM));

    CHECK(message["type"] == "telemetry");
    CHECK(message["sourceId"] == 2U);
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
    CHECK(message.size() == 16U);
}

TEST_CASE("sim raw json is the plant truth a telemetry message carries")
{
    mark4_Telemetry telemetry = asymmetricTelemetry();
    telemetry.timestamp_us = 424'242U;
    telemetry.has_truth = true;
    const float quat[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float position[3] = {-1.5f, 2.5f, 3.5f};
    const float velocity[3] = {4.5f, -5.5f, 6.5f};
    std::memcpy(telemetry.truth.attitude_quat, quat, sizeof(quat));
    std::memcpy(telemetry.truth.position_m, position, sizeof(position));
    std::memcpy(telemetry.truth.velocity_mps, velocity, sizeof(velocity));

    const nlohmann::json message = parsed(mark4::simRawToJson(telemetry, mark4_NodeKind_DRONE_SIM));
    CHECK(message["type"] == "simRaw");
    // The truth is named after the process whose estimate it accompanies:
    // the pages line the two up by that id.
    CHECK(message["sourceId"] == 2U);
    CHECK(message["timestampUs"] == 424'242U);
    CHECK(message["attitudeQuat"] == nlohmann::json({1.0, 0.5, 0.25, 0.125}));
    CHECK(message["positionM"] == nlohmann::json({-1.5, 2.5, 3.5}));
    CHECK(message["velocityMps"] == nlohmann::json({4.5, -5.5, 6.5}));
    CHECK(message.size() == 6U);
}

TEST_CASE("discovery json describes every live process")
{
    std::vector<mark4::DiscoveredProcess> processes;
    mark4::DiscoveredProcess sim;
    sim.kind = mark4_NodeKind_DRONE_SIM;
    sim.nodeId = 7U;
    sim.lastSeenUs = 1'000'000U;
    sim.name = "drone_sim";
    sim.mcu = mark4_Mcu_SIM;
    sim.wireHash = mark4::WIRE_HASH;
    processes.push_back(sim);
    mark4::DiscoveredProcess board;
    board.kind = mark4_NodeKind_FIRMWARE;
    board.lastSeenUs = 1'500'000U;
    board.name = "mark4-fc";
    board.mcu = mark4_Mcu_STM32F405;
    board.buildEpoch = 0x66E00001U;
    board.gitHash = "deadbeef";
    board.wireHash = 0x01020304U;
    board.wireMismatch = true;
    processes.push_back(board);

    std::vector<mark4::DiscoveredBridge> bridges;
    bridges.push_back({"192.168.1.31", 47830U, "c19f6c", 1'800'000U});

    const nlohmann::json message = parsed(mark4::discoveryToJson(processes, bridges, 2'000'000U));
    CHECK(message["type"] == "discovery");
    CHECK(message["wireHash"].get<std::string>().size() == 8U);
    REQUIRE(message["processes"].size() == 2U);
    CHECK(message["processes"][0]["kind"] == 2U);
    CHECK(message["processes"][0]["kindName"] == "drone_sim");
    CHECK(message["processes"][0]["sessionId"] == 7U);
    CHECK(message["processes"][0]["name"] == "drone_sim");
    CHECK(message["processes"][0]["mcu"] == 200U);
    CHECK(message["processes"][0]["wireMismatch"] == false);
    CHECK(message["processes"][0]["ageMs"] == 1000U);
    CHECK(message["processes"][1]["kindName"] == "firmware");
    CHECK(message["processes"][1]["buildEpoch"] == 0x66E00001U);
    CHECK(message["processes"][1]["gitHash"] == "deadbeef");
    CHECK(message["processes"][1]["wireHash"] == "01020304");
    CHECK(message["processes"][1]["wireMismatch"] == true);
    CHECK(message["processes"][1]["ageMs"] == 500U);
    REQUIRE(message["bridges"].size() == 1U);
    CHECK(message["bridges"][0]["address"] == "192.168.1.31");
    CHECK(message["bridges"][0]["port"] == 47830U);
    CHECK(message["bridges"][0]["name"] == "c19f6c");
    CHECK(message["bridges"][0]["ageMs"] == 200U);
}

TEST_CASE("status json carries the counters")
{
    mark4::HubStatus status;
    status.badFrames = 40U;
    status.rejectedAnnounces = 50U;
    status.clients = 3U;
    status.rcClients = 2U;
    status.connectionVia = "bridge";
    status.connectionId = "c19f6c";
    status.connectionKind = mark4_NodeKind_FIRMWARE;
    status.connectionLive = true;

    mark4::LinkHealth link;
    link.sourceId = 2U;
    link.sourceName = "drone_sim";
    link.received = 9U;
    link.lost = 1U;
    link.duplicates = 2U;
    link.lastSequence = 123U;
    status.links.push_back(link);

    const nlohmann::json message = parsed(mark4::statusToJson(status));
    CHECK(message["type"] == "status");
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
    CHECK(entry["stream"] == "transport");
    CHECK(entry["sourceId"] == 2U);
    CHECK(entry["sourceName"] == "drone_sim");
    CHECK(entry["received"] == 9U);
    CHECK(entry["lost"] == 1U);
    CHECK(entry["duplicates"] == 2U);
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

TEST_CASE("an rc message becomes the rc wire message")
{
    const mark4::ClientMessage message = decoded(
        R"({"type":"rc","id":7,"target":"firmware","kill":0,"arm":1,"mode":1,"throttle":0.5})");
    CHECK(message.type == mark4::ClientMessageType::RC);
    CHECK(message.id == 7);
    CHECK(message.target == mark4_NodeKind_FIRMWARE);
    REQUIRE(message.command.which_body == mark4_Envelope_rc_tag);
    CHECK(message.command.body.rc.kill == false);
    CHECK(message.command.body.rc.arm == true);
    CHECK(message.command.body.rc.mode == mark4_RcMode_RC_ALTITUDE_AUTO);
    CHECK(message.command.body.rc.throttle == 0.5f);

    // Booleans are accepted as well as the 0/1 of the older pages.
    const mark4::ClientMessage flags = decoded(
        R"({"type":"rc","target":"firmware","kill":true,"arm":false,"mode":0,"throttle":0})");
    CHECK(flags.command.body.rc.kill == true);
    CHECK(flags.command.body.rc.arm == false);
    CHECK(flags.command.body.rc.mode == mark4_RcMode_RC_MANUAL);
}

TEST_CASE("an rc message can target the simulator")
{
    const mark4::ClientMessage message =
        decoded(R"({"type":"rc","target":"drone_sim","kill":1,"arm":0,"mode":0,"throttle":0})");
    CHECK(message.target == mark4_NodeKind_DRONE_SIM);
    CHECK(message.id == -1);
    CHECK(message.command.body.rc.kill == true);
}

TEST_CASE("a scenario message becomes the sim scenario wire message")
{
    const mark4::ClientMessage message = decoded(
        R"({"type":"simScenario","id":3,"scenario":"handThrow","sequence":7,)"
        R"("seed":81985529216486895,"throwDelayUs":2000000,"hashWindowUs":16000000,)"
        R"("velocityMps":[1.0,2.0,3.0],"angularVelocityRadS":[4.0,5.0,6.0],)"
        R"("heldSeconds":1.5,"heldTiltRad":0.25,"heldAzimuthRad":0.5,"swingSeconds":0.375})");
    CHECK(message.type == mark4::ClientMessageType::SIM_SCENARIO);
    CHECK(message.id == 3);
    // A scenario goes to the flight process driving a plant: the simulator
    // unless the message says otherwise.
    CHECK(message.target == mark4_NodeKind_DRONE_SIM);
    REQUIRE(message.command.which_body == mark4_Envelope_sim_scenario_tag);
    const mark4_SimScenario &scenario = message.command.body.sim_scenario;
    CHECK(scenario.sequence == 7U);
    CHECK(scenario.kind == mark4_SimScenarioKind_HAND_THROW);
    CHECK(scenario.seed == 0x0123456789ABCDEFULL);
    CHECK(scenario.throw_delay_us == 2000000U);
    CHECK(scenario.hash_window_us == 16000000U);
    CHECK(scenario.velocity_mps[0] == 1.0f);
    CHECK(scenario.velocity_mps[2] == 3.0f);
    CHECK(scenario.angular_velocity_rad_s[1] == 5.0f);
    CHECK(scenario.held_seconds == 1.5f);
    CHECK(scenario.held_tilt_rad == 0.25f);
    CHECK(scenario.held_azimuth_rad == 0.5f);
    CHECK(scenario.swing_seconds == 0.375f);
}

TEST_CASE("a reset scenario message needs nothing but its name")
{
    const mark4::ClientMessage message = decoded(R"({"type":"simScenario","scenario":"reset"})");
    CHECK(message.command.body.sim_scenario.kind == mark4_SimScenarioKind_RESET);
    // Left at 0, so the hub stamps its own number before sending.
    CHECK(message.command.body.sim_scenario.sequence == 0U);

    const mark4::ClientMessage thrown = decoded(R"({"type":"simScenario","scenario":"throw"})");
    CHECK(thrown.command.body.sim_scenario.kind == mark4_SimScenarioKind_THROW);
}

TEST_CASE("a reboot message becomes the reboot wire message")
{
    const mark4::ClientMessage message =
        decoded(R"({"type":"reboot","id":11,"target":"firmware"})");
    CHECK(message.type == mark4::ClientMessageType::REBOOT);
    CHECK(message.command.which_body == mark4_Envelope_reboot_tag);
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
    const mark4::ClientMessage udp =
        decoded(R"({"type":"connect","via":"udp","target":"drone_sim"})");
    CHECK(udp.type == mark4::ClientMessageType::CONNECT);
    CHECK(udp.connectVia == "udp");
    CHECK(udp.target == mark4_NodeKind_DRONE_SIM);

    const mark4::ClientMessage bridge =
        decoded(R"({"type":"connect","via":"bridge","name":"c19f6c"})");
    CHECK(bridge.connectVia == "bridge");
    CHECK(bridge.connectPeer == "c19f6c");

    const mark4::ClientMessage disconnect = decoded(R"({"type":"disconnect"})");
    CHECK(disconnect.type == mark4::ClientMessageType::DISCONNECT);

    // Each route requires its identity: no name and no target means there
    // is nothing to connect to
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"udp"})")));
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"bridge"})")));
    CHECK(std::holds_alternative<std::string>(
        mark4::parseClientMessage(R"({"type":"connect","via":"carrier-pigeon"})")));
}

TEST_CASE("a tuning set message becomes the tuning set wire message")
{
    const mark4::ClientMessage message =
        decoded(R"({"type":"tuningSet","id":7,"target":"drone_sim","paramId":101,"value":0.028})");
    CHECK(message.type == mark4::ClientMessageType::TUNING_SET);
    CHECK(message.id == 7);
    CHECK(message.target == mark4_NodeKind_DRONE_SIM);
    REQUIRE(message.command.which_body == mark4_Envelope_tuning_set_tag);
    CHECK(message.command.body.tuning_set.id == 101U);
    CHECK(message.command.body.tuning_set.value == 0.028f);
}

TEST_CASE("a tuning list becomes its wire message")
{
    // startIndex is optional and defaults to the top of the table.
    const mark4::ClientMessage list =
        decoded(R"({"type":"tuningList","id":9,"target":"drone_sim"})");
    CHECK(list.type == mark4::ClientMessageType::TUNING_LIST);
    REQUIRE(list.command.which_body == mark4_Envelope_tuning_list_tag);
    CHECK(list.command.body.tuning_list.start_index == 0U);

    const mark4::ClientMessage paged =
        decoded(R"({"type":"tuningList","target":"drone_sim","startIndex":4})");
    CHECK(paged.command.body.tuning_list.start_index == 4U);
}

TEST_CASE("tuning ack json names the status it carries")
{
    mark4_TuningAck ack = mark4_TuningAck_init_zero;
    ack.id = 101U;
    ack.value = 0.028f;
    ack.status = mark4_TuningStatus_OK;

    const nlohmann::json message = parsed(mark4::tuningAckToJson(ack, mark4_NodeKind_DRONE_SIM));
    CHECK(message["type"] == "tuningAck");
    CHECK(message["source"] == "drone_sim");
    // The parameter id key is never "id": that one is the correlation id.
    CHECK(message["paramId"] == 101U);
    CHECK(message["value"] == 0.028f);
    CHECK(message["status"] == 0U);
    CHECK(message["statusName"] == "ok");
    CHECK(message.size() == 6U);

    const char *names[] = {"ok", "unknownId", "outOfBounds", "lockedWhileArmed", "unknown"};
    for (int status = 0; status < 5; ++status)
    {
        ack.status = static_cast<mark4_TuningStatus>(status);
        const nlohmann::json named = parsed(mark4::tuningAckToJson(ack, mark4_NodeKind_FIRMWARE));
        INFO("status " << status);
        CHECK(named["statusName"] == names[status]);
        CHECK(named["source"] == "firmware");
    }
}

TEST_CASE("tuning info json carries the description")
{
    mark4_TuningInfo info = mark4_TuningInfo_init_zero;
    info.index = 3U;
    info.count = 12U;
    info.id = 401U;
    static_cast<void>(std::snprintf(info.name, sizeof(info.name), "%s", "ahrs_kp"));
    info.value = 2.0f;
    info.min_value = 0.5f;
    info.max_value = 8.0f;
    info.armed_change = false;

    const nlohmann::json message = parsed(mark4::tuningInfoToJson(info, mark4_NodeKind_DRONE_SIM));
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

    // A name filling the whole wire field.
    static_cast<void>(std::snprintf(info.name, sizeof(info.name), "%s", "xxxxxxxxxxxxxxxx"));
    info.armed_change = true;
    const nlohmann::json full = parsed(mark4::tuningInfoToJson(info, mark4_NodeKind_DRONE_SIM));
    CHECK(full["name"] == std::string(16U, 'x'));
    CHECK(full["armedChange"] == true);
}

TEST_CASE("profile messages decode and render")
{
    const mark4::ClientMessage list = decoded(R"({"type":"profileList","id":10})");
    CHECK(list.type == mark4::ClientMessageType::PROFILE_LIST);

    const mark4::ClientMessage save = decoded(
        R"({"type":"profileSave","id":11,"name":"bench","values":{"101":0.028,"303":0.55}})");
    CHECK(save.type == mark4::ClientMessageType::PROFILE_SAVE);
    CHECK(save.profileName == "bench");
    REQUIRE(save.profileValues.size() == 2U);
    CHECK(save.profileValues.at(101U) == 0.028f);
    CHECK(save.profileValues.at(303U) == 0.55f);

    const mark4::ClientMessage load = decoded(R"({"type":"profileLoad","id":12,"name":"bench"})");
    CHECK(load.profileName == "bench");

    const mark4::ClientMessage push =
        decoded(R"({"type":"profilePush","id":13,"name":"bench","target":"drone_sim"})");
    CHECK(push.type == mark4::ClientMessageType::PROFILE_PUSH);
    CHECK(push.target == mark4_NodeKind_DRONE_SIM);

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
        const auto result = mark4::parseClientMessage(text);
        INFO("message: " << text);
        REQUIRE(std::holds_alternative<std::string>(result));
        CHECK(!(std::get<std::string>(result).empty()));
    }
}

TEST_CASE("a rejected message can still be answered by its id")
{
    CHECK(mark4::clientMessageId(R"({"type":"rc","id":12,"throttle":"fast"})") == 12);
    CHECK(mark4::clientMessageId(R"({"type":"rc","throttle":"fast"})") == -1);
    CHECK(mark4::clientMessageId("not json at all") == -1);
}
