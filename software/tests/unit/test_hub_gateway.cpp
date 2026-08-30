/// @file
/// @brief The gateway side of gateway.proto: messages round trip through the
///        codec, the update client snapshot reads like the client, an update
///        command fixes its target for the session, a profile push is one
///        TuningSet per value to the node named.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "hub/gateway_codec.hpp"
#include "hub/ota_client.hpp"
#include "hub/tuning_profiles.hpp"

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

TEST_CASE("a frame and its correlation id round trip through the gateway codec")
{
    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_frame_tag;
    message.id = 0x12345U;
    message.body.frame.src = 7U;
    message.body.frame.dst = 9U;
    mark4_Envelope rc = mark4_Envelope_init_zero;
    rc.which_body = mark4_Envelope_rc_tag;
    rc.body.rc.arm = true;
    rc.body.rc.throttle = 0.5F;
    std::size_t size = 0U;
    REQUIRE(mark4::encodeEnvelope(
        rc, message.body.frame.payload.bytes, sizeof(message.body.frame.payload.bytes), size));
    message.body.frame.payload.size = static_cast<pb_size_t>(size);

    const mark4_GatewayMessage decoded = roundTrip(message);
    CHECK(decoded.which_body == mark4_GatewayMessage_frame_tag);
    CHECK(decoded.id == 0x12345U);
    CHECK(decoded.body.frame.src == 7U);
    CHECK(decoded.body.frame.dst == 9U);
    mark4_Envelope back;
    REQUIRE(mark4::decodeEnvelope(
        decoded.body.frame.payload.bytes, decoded.body.frame.payload.size, back));
    CHECK(back.which_body == mark4_Envelope_rc_tag);
    CHECK(back.body.rc.arm);
    CHECK(back.body.rc.throttle == 0.5F);

    mark4_GatewayMessage empty = mark4_GatewayMessage_init_zero;
    std::string bytes;
    CHECK(!mark4::encodeGatewayMessage(empty, bytes));
    CHECK(!mark4::decodeGatewayMessage(nullptr, 0U, empty));
}

TEST_CASE("the node table carries the transport record and the last announce")
{
    mark4::Transport::Node node;
    node.id = 0xABCDU;
    node.address.host = 0xC0A80105U; // 192.168.1.5
    node.address.port = 4711U;
    node.lastSeenUs = 1'000'000U;
    node.received = 10U;
    node.lost = 2U;
    node.duplicates = 1U;
    mark4_Announce announce = mark4_Announce_init_zero;
    announce.kind = mark4_NodeKind_DRONE_SIM;
    announce.wire_hash = 0xDEADBEEFU;
    mark4::copyWireString("sim", announce.name, sizeof(announce.name));

    // The table as two pages: a page sizes the table to the total it
    // announces and lands at its own index.
    mark4::LogModuleTable modules;
    mark4_LogModules page = mark4_LogModules_init_zero;
    page.start_index = 0U;
    page.total = 2U;
    page.modules_count = 1U;
    page.modules[0].id = 16U;
    mark4::copyWireString("platform/imu", page.modules[0].name, sizeof(page.modules[0].name));
    page.modules[0].level = mark4_LogLevel_INFO;
    mark4::applyLogModulesPage(page, modules);
    page.start_index = 1U;
    page.modules[0].id = 17U;
    mark4::copyWireString("platform/baro", page.modules[0].name, sizeof(page.modules[0].name));
    page.modules[0].level = mark4_LogLevel_DEBUG;
    mark4::applyLogModulesPage(page, modules);
    REQUIRE(modules.size() == 2U);

    mark4_GatewayMessage message = mark4_GatewayMessage_init_zero;
    message.which_body = mark4_GatewayMessage_nodes_tag;
    mark4::fillNode(node, 1'250'000U, &announce, modules, message.body.nodes.nodes[0]);
    mark4::fillNode(node, 1'250'000U, nullptr, {}, message.body.nodes.nodes[1]);
    message.body.nodes.nodes_count = 2U;

    const mark4_GatewayMessage decoded = roundTrip(message);
    REQUIRE(decoded.body.nodes.nodes_count == 2U);
    const mark4_Node &first = decoded.body.nodes.nodes[0];
    CHECK(first.id == 0xABCDU);
    CHECK(std::string(first.address) == "192.168.1.5");
    CHECK(first.port == 4711U);
    CHECK(first.last_seen_ms_ago == 250U);
    CHECK(first.received == 10U);
    CHECK(first.lost == 2U);
    CHECK(first.duplicates == 1U);
    REQUIRE(first.has_announce);
    CHECK(first.announce.kind == mark4_NodeKind_DRONE_SIM);
    CHECK(first.announce.wire_hash == 0xDEADBEEFU);
    CHECK(std::string(first.announce.name) == "sim");
    REQUIRE(first.log_modules_count == 2U);
    CHECK(first.log_modules[0].id == 16U);
    CHECK(std::string(first.log_modules[0].name) == "platform/imu");
    CHECK(first.log_modules[1].id == 17U);
    CHECK(first.log_modules[1].level == mark4_LogLevel_DEBUG);
    CHECK(!decoded.body.nodes.nodes[1].has_announce);
    CHECK(decoded.body.nodes.nodes[1].log_modules_count == 0U);

    // A page opening at 0 restarts the table: a rebooted node with fewer
    // modules does not keep stale entries.
    page.start_index = 0U;
    page.total = 1U;
    mark4::applyLogModulesPage(page, modules);
    REQUIRE(modules.size() == 1U);
    CHECK(modules[0].id == 17U);
    CHECK(mark4::hexNodeId(0xABCDU) == "0000abcd");
}

TEST_CASE("the update state snapshot reads like the client")
{
    mark4::OtaClient client;
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
    mark4::OtaClient client;
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

    std::vector<std::pair<std::uint32_t, mark4_TuningSet>> sent;
    const mark4::EnvelopeSink sink =
        [&sent](std::uint32_t dst, const mark4_Envelope &envelope, std::string &) {
            REQUIRE(envelope.which_body == mark4_Envelope_tuning_set_tag);
            sent.emplace_back(dst, envelope.body.tuning_set);
            return true;
        };
    CHECK(!mark4::pushProfile(profiles, "bench", 0U, sink, error));
    CHECK(error == "no target node");
    CHECK(!mark4::pushProfile(profiles, "missing", 9U, sink, error));
    CHECK(sent.empty());

    REQUIRE(mark4::pushProfile(profiles, "bench", 9U, sink, error));
    REQUIRE(sent.size() == 2U);
    CHECK(sent[0].first == 9U);
    CHECK(sent[0].second.id == 101U);
    CHECK(sent[0].second.value == 0.25F);
    CHECK(sent[1].second.id == 102U);
    CHECK(sent[1].second.value == 2.0F);

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
