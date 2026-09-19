/// @file
/// @brief The gateway side of gateway.proto: typed messages round trip
///        through the codec in both directions, the update client snapshot
///        reads like the client, an update command fixes its target for the
///        session, a profile push is one write per value to the node named.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "hub/gateway_codec.hpp"
#include "hub/tuning_profiles.hpp"
#include "log/consumer.hpp"
#include "messaging/table_pull.hpp"
#include "ota/consumer.hpp"
#include "telemetry/registry.hpp"

namespace
{
    /// A directory of its own per test, removed on the way out.
    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            std::error_code code;
            m_path = std::filesystem::temp_directory_path(code) /
                     ("mark4_gateway_" + std::to_string(::getpid()));
            static_cast<void>(std::filesystem::remove_all(m_path, code));
        }

        ScratchDirectory(const ScratchDirectory &) = delete;
        ScratchDirectory &operator=(const ScratchDirectory &) = delete;
        ScratchDirectory(ScratchDirectory &&) = delete;
        ScratchDirectory &operator=(ScratchDirectory &&) = delete;

        ~ScratchDirectory()
        {
            std::error_code code;
            static_cast<void>(std::filesystem::remove_all(m_path, code));
        }

        [[nodiscard]] std::string path() const
        {
            return m_path.string();
        }

      private:
        std::filesystem::path m_path; ///< directory owned by this test
    };

    /// @brief Encodes then decodes one message.
    mark4_GatewayMessage roundTrip(const mark4_GatewayMessage &message)
    {
        std::string bytes;
        REQUIRE(mark4::encodeGatewayMessage(message, bytes));
        mark4_GatewayMessage decoded;
        REQUIRE(mark4::decodeGatewayMessage(
            reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), decoded));
        return decoded;
    }
} // namespace

TEST_CASE("a typed message and its correlation id round trip through the gateway codec")
{
    // Both directions through the one codec: what the gateway publishes of
    // a node it hears, and what a client commands of a node it names.
    mark4_GatewayMessage published = mark4_GatewayMessage_init_zero;
    published.which_body = mark4_GatewayMessage_node_status_tag;
    published.id = 0x12345U;
    published.body.node_status.node = 7U;
    published.body.node_status.has_status = true;
    published.body.node_status.status.flight_phase = mark4_FlightPhase_PHASE_HOVER;
    published.body.node_status.status.throw_count = 3U;
    published.body.node_status.status.imu_valid = true;

    const mark4_GatewayMessage decoded = roundTrip(published);
    CHECK(decoded.which_body == mark4_GatewayMessage_node_status_tag);
    CHECK(decoded.id == 0x12345U);
    CHECK(decoded.body.node_status.node == 7U);
    REQUIRE(decoded.body.node_status.has_status);
    CHECK(decoded.body.node_status.status.flight_phase == mark4_FlightPhase_PHASE_HOVER);
    CHECK(decoded.body.node_status.status.throw_count == 3U);
    CHECK(decoded.body.node_status.status.imu_valid);

    mark4_GatewayMessage command = mark4_GatewayMessage_init_zero;
    command.which_body = mark4_GatewayMessage_telemetry_command_tag;
    command.id = 0x777U;
    command.body.telemetry_command.node = 9U;
    command.body.telemetry_command.which_action = mark4_TelemetryCommand_config_tag;
    mark4_TelemetryConfigRequest &config = command.body.telemetry_command.action.config;
    config.ids_count = 2U;
    config.ids[0] = 4U;
    config.ids[1] = 7U;
    config.period_ms = 50U;

    const mark4_GatewayMessage commanded = roundTrip(command);
    CHECK(commanded.which_body == mark4_GatewayMessage_telemetry_command_tag);
    CHECK(commanded.id == 0x777U);
    CHECK(commanded.body.telemetry_command.node == 9U);
    REQUIRE(commanded.body.telemetry_command.which_action == mark4_TelemetryCommand_config_tag);
    REQUIRE(commanded.body.telemetry_command.action.config.ids_count == 2U);
    CHECK(commanded.body.telemetry_command.action.config.ids[1] == 7U);
    CHECK(commanded.body.telemetry_command.action.config.period_ms == 50U);

    mark4_GatewayMessage empty = mark4_GatewayMessage_init_zero;
    std::string bytes;
    CHECK(!mark4::encodeGatewayMessage(empty, bytes));
    CHECK(!mark4::decodeGatewayMessage(nullptr, 0U, empty));
}

TEST_CASE("the node table carries the transport record and the last announce")
{
    mark4::Transport::Node node;
    node.id = 0xABCDU;
    node.address = mark4::UdpAddress{0xC0A80105U, 4711U}; // 192.168.1.5
    node.lastSeenUs = 1'000'000U;
    mark4_Announce announce = mark4_Announce_init_zero;
    announce.kind = mark4_NodeKind_DRONE_SIM;
    announce.wire_hash = 0xDEADBEEFU;
    mark4::copyWireString("sim", announce.name, sizeof(announce.name));

    // The table as two pages: every page says the total, lands at the
    // cursor the walk waits on and moves it on by its own item count.
    mark4::TablePull<mark4_LogModuleInfo, mark4::LogConsumerBase::MAX_MODULES> modules;
    mark4_LogModules page = mark4_LogModules_init_zero;
    page.cursor = 0U;
    page.total = 2U;
    page.modules_count = 1U;
    page.modules[0].id = 16U;
    mark4::copyWireString("platform/imu", page.modules[0].name, sizeof(page.modules[0].name));
    page.modules[0].level = mark4_LogLevel_INFO;
    CHECK(modules.applyPage(page.total, page.cursor, {page.modules, page.modules_count}));
    CHECK(modules.total() == 2U);
    CHECK(modules.cursor() == 1U);
    CHECK(!modules.complete());
    page.cursor = 1U;
    page.modules[0].id = 17U;
    mark4::copyWireString("platform/baro", page.modules[0].name, sizeof(page.modules[0].name));
    page.modules[0].level = mark4_LogLevel_DEBUG;
    CHECK(modules.applyPage(page.total, page.cursor, {page.modules, page.modules_count}));
    CHECK(modules.complete());
    REQUIRE(modules.size() == 2U);
    // The same page again is a duplicate or a stale answer: the walk waits
    // on cursor 2 now and the table is left alone.
    CHECK(!modules.applyPage(page.total, page.cursor, {page.modules, page.modules_count}));
    CHECK(modules.size() == 2U);

    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_nodes_tag;
    mark4::fillNode(node, 1'250'000U, &announce, message.body.nodes.nodes[0]);
    mark4::fillNode(node, 1'250'000U, nullptr, message.body.nodes.nodes[1]);
    message.body.nodes.nodes_count = 2U;

    const mark4_GatewayMessage decoded = roundTrip(message);
    REQUIRE(decoded.body.nodes.nodes_count == 2U);
    const mark4_Node &first = decoded.body.nodes.nodes[0];
    CHECK(first.id == 0xABCDU);
    CHECK(std::string(first.address) == "192.168.1.5");
    CHECK(first.port == 4711U);
    CHECK(first.last_seen_ms_ago == 250U);
    REQUIRE(first.has_announce);
    CHECK(first.announce.kind == mark4_NodeKind_DRONE_SIM);
    CHECK(first.announce.wire_hash == 0xDEADBEEFU);
    CHECK(std::string(first.announce.name) == "sim");
    CHECK(!decoded.body.nodes.nodes[1].has_announce);

    // The table the walk filled is published on its own, out of Node: the
    // clients read the modules of one node there.
    mark4_GatewayMessage lines = mark4_GatewayMessage_init_zero;
    lines.which_body = mark4_GatewayMessage_node_log_modules_tag;
    mark4::fillNodeLogModules(node.id, modules.items(), lines.body.node_log_modules);
    const mark4_NodeLogModules &tableOut = roundTrip(lines).body.node_log_modules;
    CHECK(tableOut.node == 0xABCDU);
    REQUIRE(tableOut.modules_count == 2U);
    CHECK(tableOut.modules[0].id == 16U);
    CHECK(std::string(tableOut.modules[0].name) == "platform/imu");
    CHECK(tableOut.modules[1].id == 17U);
    CHECK(tableOut.modules[1].level == mark4_LogLevel_DEBUG);
    // A node the gateway holds nothing of publishes an empty table, which
    // is also what a node going down publishes.
    mark4::fillNodeLogModules(node.id, {}, lines.body.node_log_modules);
    CHECK(roundTrip(lines).body.node_log_modules.modules_count == 0U);

    // A walk started again opens at cursor 0 and the page there restarts
    // the table: a rebooted node with fewer modules keeps no stale entry.
    modules.reset();
    page.cursor = 0U;
    page.total = 1U;
    CHECK(modules.applyPage(page.total, page.cursor, {page.modules, page.modules_count}));
    REQUIRE(modules.size() == 1U);
    CHECK(modules.complete());
    CHECK(modules.items()[0].id == 17U);
    CHECK(mark4::hexNodeId(0xABCDU) == "0000abcd");
}

TEST_CASE("a telemetry table is merged page by page and published on its own")
{
    // The walk the gateway runs against a drone: one request per page, the
    // cursor the merge returns, and the total closing it. The live pull
    // itself (a hub asking a real drone_sim) is what scripts/smoke.ts
    // checks; this is the merge it is built on.
    mark4::TablePull<mark4_TelemetryDescriptor, mark4::MAX_TELEMETRY_ENTRIES> table;
    mark4_TelemetryDescriptors page = mark4_TelemetryDescriptors_init_zero;
    page.total = 3U;
    page.cursor = 0U;
    page.descriptors_count = 2U;
    page.descriptors[0].id = 0U;
    mark4::copyWireString(
        "sensor/gyro_x", page.descriptors[0].name, sizeof(page.descriptors[0].name));
    page.descriptors[0].unit = mark4_TelemetryUnit_TELEMETRY_UNIT_RAD_PER_S;
    page.descriptors[1].id = 1U;
    mark4::copyWireString(
        "estimator/altitude", page.descriptors[1].name, sizeof(page.descriptors[1].name));
    page.descriptors[1].unit = mark4_TelemetryUnit_TELEMETRY_UNIT_M;
    CHECK(table.applyPage(page.total, page.cursor, {page.descriptors, page.descriptors_count}));
    CHECK(table.cursor() == 2U);
    CHECK(table.total() == 3U);
    CHECK(!table.complete());

    page.cursor = 2U;
    page.descriptors_count = 1U;
    page.descriptors[0].id = 2U;
    mark4::copyWireString(
        "mixer/motor_0", page.descriptors[0].name, sizeof(page.descriptors[0].name));
    page.descriptors[0].unit = mark4_TelemetryUnit_TELEMETRY_UNIT_UNITLESS;
    // The cursor reaches the total: the table is whole and the walk stops.
    CHECK(table.applyPage(page.total, page.cursor, {page.descriptors, page.descriptors_count}));
    CHECK(table.cursor() == 3U);
    CHECK(table.complete());

    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_node_telemetry_tag;
    mark4::fillNodeTelemetry(0xABCDU, table.items(), message.body.node_telemetry);
    const mark4_GatewayMessage decoded = roundTrip(message);
    const mark4_NodeTelemetry &published = decoded.body.node_telemetry;
    CHECK(published.node == 0xABCDU);
    REQUIRE(published.descriptors_count == 3U);
    CHECK(std::string(published.descriptors[0].name) == "sensor/gyro_x");
    CHECK(published.descriptors[1].unit == mark4_TelemetryUnit_TELEMETRY_UNIT_M);
    CHECK(std::string(published.descriptors[2].name) == "mixer/motor_0");

    // A walk started again opens at cursor 0 and the page there restarts
    // the table: a rebooted node with fewer measures keeps no stale entry,
    // and its ids are its own.
    table.reset();
    page.cursor = 0U;
    page.total = 1U;
    page.descriptors_count = 1U;
    CHECK(table.applyPage(page.total, page.cursor, {page.descriptors, page.descriptors_count}));
    REQUIRE(table.size() == 1U);
    CHECK(table.complete());
    CHECK(std::string(table.items()[0].name) == "mixer/motor_0");

    // An empty page while the table is not full would loop the walk forever
    // on the same cursor: it closes the walk instead.
    mark4::TablePull<mark4_TelemetryDescriptor, mark4::MAX_TELEMETRY_ENTRIES> stalled;
    page.total = 10U;
    page.cursor = 0U;
    page.descriptors_count = 4U;
    CHECK(stalled.applyPage(page.total, page.cursor, {page.descriptors, page.descriptors_count}));
    CHECK(!stalled.complete());
    page.cursor = 4U;
    page.descriptors_count = 0U;
    CHECK(stalled.applyPage(page.total, page.cursor, {page.descriptors, page.descriptors_count}));
    CHECK(stalled.complete());

    // A node that exposes nothing publishes an empty table, which is also
    // what a node going down publishes.
    mark4::fillNodeTelemetry(0xABCDU, {}, message.body.node_telemetry);
    CHECK(roundTrip(message).body.node_telemetry.descriptors_count == 0U);
}

TEST_CASE("the update state snapshot reads like the client")
{
    mark4::OtaConsumer client;
    client.setDefaultBundlePath("/nowhere/drone_firmware.ota");
    const mark4_OtaState idle = mark4::otaStateOf(client, 0U);
    CHECK(idle.phase == mark4_OtaState_Phase_IDLE);
    CHECK(idle.verdict == mark4_OtaState_Verdict_VERDICT_NONE);
    CHECK(idle.target_node == 0U);
    CHECK(idle.target_slot == -1);
    REQUIRE(idle.has_bundle);
    CHECK(!idle.bundle.loaded);
    CHECK(std::string(idle.bundle.path) == "/nowhere/drone_firmware.ota");
    REQUIRE(idle.has_board);
    CHECK(!idle.board.seen);
    CHECK(idle.board.slots_count == 0U);
    REQUIRE(idle.has_progress);
    CHECK(idle.progress.total_bytes == 0U);

    // A start with no sink is refused and leaves the failure readable.
    std::string error;
    CHECK(!client.start("", 1U, error));
    const mark4_OtaState failed = mark4::otaStateOf(client, 42U);
    CHECK(failed.target_node == 42U);
    CHECK(std::string(failed.last_error) == client.lastError());

    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_ota_state_tag;
    message.body.ota_state = failed;
    const mark4_GatewayMessage decoded = roundTrip(message);
    CHECK(std::string(decoded.body.ota_state.bundle.path) == "/nowhere/drone_firmware.ota");
}

TEST_CASE("an update command needs a target and keeps it for the session")
{
    mark4::OtaConsumer client;
    std::vector<mark4_Envelope> sent;
    client.setSink([&sent](const mark4_Envelope &envelope, std::string &) {
        sent.push_back(envelope);
        return true;
    });
    std::uint32_t target = 0U;
    std::string error;

    mark4_OtaCommand command = mark4_OtaCommand_init_zero;
    command.op = mark4_OtaCommand_Op_STATUS_REQUEST;
    CHECK(!mark4::applyOtaCommand(client, command, target, 1U, error));
    CHECK(error == "no target node");
    CHECK(sent.empty());

    command.target_node = 5U;
    REQUIRE(mark4::applyOtaCommand(client, command, target, 1U, error));
    CHECK(target == 5U);
    REQUIRE(sent.size() == 1U);
    CHECK(sent[0].which_body == mark4_Envelope_ota_status_request_tag);

    // A start opens a session (the query phase) against that node; a
    // command naming another node is then refused, an abort is not.
    command.op = mark4_OtaCommand_Op_START;
    mark4::copyWireString("/nowhere.ota", command.bundle_path, sizeof(command.bundle_path));
    CHECK(!mark4::applyOtaCommand(client, command, target, 2U, error)); // no bundle there
    CHECK(!client.busy());
    CHECK(target == 5U);

    command.op = mark4_OtaCommand_Op_STATUS_REQUEST;
    command.target_node = 6U;
    REQUIRE(mark4::applyOtaCommand(client, command, target, 3U, error));
    CHECK(target == 6U); // idle again: the target may move
    command.op = mark4_OtaCommand_Op_ABORT;
    command.target_node = 0U;
    static_cast<void>(mark4::applyOtaCommand(client, command, target, 4U, error));
    CHECK(target == 6U);
}

TEST_CASE("a profile push is one TuningSet per value to the node named")
{
    const ScratchDirectory scratch;
    mark4::TuningProfiles profiles(scratch.path());
    std::string error;
    REQUIRE(profiles.save("bench", {{101U, 0.25F}, {102U, 2.0F}}, error));

    std::vector<std::pair<std::uint32_t, float>> sent;
    const mark4::TuningSink sink = [&sent](std::uint32_t id, float value) {
        sent.emplace_back(id, value);
        return true;
    };
    CHECK(!mark4::pushProfile(profiles, "bench", 0U, sink, error));
    CHECK(error == "no target node");
    CHECK(!mark4::pushProfile(profiles, "missing", 9U, sink, error));
    CHECK(sent.empty());

    REQUIRE(mark4::pushProfile(profiles, "bench", 9U, sink, error));
    REQUIRE(sent.size() == 2U);
    CHECK(sent[0].first == 101U);
    CHECK(sent[0].second == 0.25F);
    CHECK(sent[1].first == 102U);
    CHECK(sent[1].second == 2.0F);

    // A node that takes no write stops the push where it got to.
    sent.clear();
    const mark4::TuningSink refusing = [](std::uint32_t, float) { return false; };
    CHECK(!mark4::pushProfile(profiles, "bench", 9U, refusing, error));

    // The profile itself, as a client reads it back.
    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_profile_tag;
    mark4::TuningValues values;
    REQUIRE(profiles.load("bench", values, error));
    mark4::fillProfile("bench", values, message.body.profile);
    const mark4_GatewayMessage decoded = roundTrip(message);
    CHECK(std::string(decoded.body.profile.name) == "bench");
    REQUIRE(decoded.body.profile.values_count == 2U);
    CHECK(decoded.body.profile.values[1].id == 102U);
    const mark4::TuningValues back =
        mark4::tuningValuesOf(decoded.body.profile.values, decoded.body.profile.values_count);
    CHECK(back == values);
}
