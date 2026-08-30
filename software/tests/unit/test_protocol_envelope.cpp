/// @file
/// @brief The wire codec: every body of the Envelope encodes and decodes
///        back to itself, the bounds of mark4.options hold, and the
///        decoder refuses what is not an Envelope.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "protocol/envelope.hpp"
#include "transport/frame.hpp"
#include "transport/serial_framing.hpp"

namespace
{
    /// @brief Encodes then decodes one envelope.
    /// @param envelope message to round trip
    /// @param[out] sizeOut encoded size
    /// @return the decoded message
    mark4_Envelope roundTrip(const mark4_Envelope &envelope, std::size_t &sizeOut)
    {
        std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
        REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), sizeOut));
        REQUIRE(sizeOut > 0U);
        REQUIRE(sizeOut <= mark4::MAX_ENVELOPE_SIZE);
        mark4_Envelope decoded;
        REQUIRE(mark4::decodeEnvelope(bytes.data(), sizeOut, decoded));
        REQUIRE(decoded.which_body == envelope.which_body);
        return decoded;
    }

    /// @brief Builds an envelope holding one body.
    /// @param tag mark4_Envelope_*_tag of the body
    /// @return the envelope, body zeroed
    mark4_Envelope withBody(pb_size_t tag)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = tag;
        return envelope;
    }

    /// @brief Fills a float array with distinct values.
    /// @tparam N element count
    /// @param values array to fill
    /// @param offset first value
    template <std::size_t N> void fill(float (&values)[N], float offset)
    {
        for (std::size_t index = 0U; index < N; ++index)
        {
            values[index] = offset + static_cast<float>(index) * 0.25f;
        }
    }

    /// @brief Compares two float arrays element by element.
    /// @tparam N element count
    /// @param left one array
    /// @param right the other
    /// @return true when every element is equal
    template <std::size_t N> bool same(const float (&left)[N], const float (&right)[N])
    {
        for (std::size_t index = 0U; index < N; ++index)
        {
            if (left[index] != right[index])
            {
                return false;
            }
        }
        return true;
    }
} // namespace

TEST_CASE("every envelope fits the links it travels on")
{
    STATIC_REQUIRE(mark4::MAX_ENVELOPE_SIZE <= mark4::MAX_PAYLOAD);
    STATIC_REQUIRE(mark4::MAX_ENVELOPE_SIZE <= mark4::SERIAL_MAX_PAYLOAD);
    STATIC_REQUIRE(sizeof(mark4_Envelope) < 400U);
    // The wire hash is a build fact, never zero.
    STATIC_REQUIRE(mark4::WIRE_HASH != 0U);
}

TEST_CASE("telemetry round trips, truth included")
{
    mark4_Envelope envelope = withBody(mark4_Envelope_telemetry_tag);
    mark4_Telemetry &telemetry = envelope.body.telemetry;
    telemetry.timestamp_us = 9'876'543'210U;
    fill(telemetry.gyro_rad_s, 1.0f);
    fill(telemetry.attitude_quat, 2.0f);
    fill(telemetry.gyro_bias_rad_s, 3.0f);
    fill(telemetry.motor, 4.0f);
    telemetry.altitude_m = 12.5f;
    telemetry.baro_altitude_m = 11.75f;
    telemetry.vertical_velocity_mps = -3.25f;
    telemetry.flight_phase = mark4_FlightPhase_PHASE_RECOVERY;
    telemetry.throw_state = mark4_ThrowState_THROW_BALLISTIC;
    telemetry.throw_count = 5U;
    telemetry.release_velocity_mps = 6.75f;
    telemetry.apex_timestamp_us = 1'234'567'890U;
    telemetry.apex_altitude_m = 8.5f;
    telemetry.has_truth = true;
    fill(telemetry.truth.attitude_quat, 5.0f);
    fill(telemetry.truth.position_m, 6.0f);
    fill(telemetry.truth.velocity_mps, 7.0f);

    std::size_t size = 0U;
    const mark4_Envelope decoded = roundTrip(envelope, size);
    const mark4_Telemetry &back = decoded.body.telemetry;
    CHECK(back.timestamp_us == 9'876'543'210U);
    CHECK(same(back.gyro_rad_s, telemetry.gyro_rad_s));
    CHECK(same(back.attitude_quat, telemetry.attitude_quat));
    CHECK(same(back.gyro_bias_rad_s, telemetry.gyro_bias_rad_s));
    CHECK(same(back.motor, telemetry.motor));
    CHECK(back.altitude_m == 12.5f);
    CHECK(back.baro_altitude_m == 11.75f);
    CHECK(back.vertical_velocity_mps == -3.25f);
    CHECK(back.flight_phase == mark4_FlightPhase_PHASE_RECOVERY);
    CHECK(back.throw_state == mark4_ThrowState_THROW_BALLISTIC);
    CHECK(back.throw_count == 5U);
    CHECK(back.release_velocity_mps == 6.75f);
    CHECK(back.apex_timestamp_us == 1'234'567'890U);
    CHECK(back.apex_altitude_m == 8.5f);
    REQUIRE(back.has_truth);
    CHECK(same(back.truth.attitude_quat, telemetry.truth.attitude_quat));
    CHECK(same(back.truth.position_m, telemetry.truth.position_m));
    CHECK(same(back.truth.velocity_mps, telemetry.truth.velocity_mps));

    // Without the truth the message is shorter, and says so.
    telemetry.has_truth = false;
    std::size_t bare = 0U;
    CHECK(!roundTrip(envelope, bare).body.telemetry.has_truth);
    CHECK(bare < size);
}

TEST_CASE("the sim link messages round trip")
{
    mark4_Envelope sensor = withBody(mark4_Envelope_sim_sensor_tag);
    sensor.body.sim_sensor.timestamp_us = 4'000U;
    fill(sensor.body.sim_sensor.gyro_rad_s, 0.5f);
    fill(sensor.body.sim_sensor.accel_mps2, 9.0f);
    sensor.body.sim_sensor.baro_pa = 101325.0f;
    sensor.body.sim_sensor.reset_count = 3U;
    sensor.body.sim_sensor.lockstep_timeouts = 517U;
    sensor.body.sim_sensor.has_truth = true;
    sensor.body.sim_sensor.truth.attitude_quat[0] = 1.0f;
    std::size_t size = 0U;
    const mark4_SimSensor &backSensor = roundTrip(sensor, size).body.sim_sensor;
    CHECK(backSensor.timestamp_us == 4'000U);
    CHECK(same(backSensor.gyro_rad_s, sensor.body.sim_sensor.gyro_rad_s));
    CHECK(same(backSensor.accel_mps2, sensor.body.sim_sensor.accel_mps2));
    CHECK(backSensor.baro_pa == 101325.0f);
    CHECK(backSensor.reset_count == 3U);
    CHECK(backSensor.lockstep_timeouts == 517U);
    CHECK(backSensor.has_truth);
    CHECK(backSensor.truth.attitude_quat[0] == 1.0f);

    mark4_Envelope actuator = withBody(mark4_Envelope_sim_actuator_tag);
    actuator.body.sim_actuator.echo_timestamp_us = 4'000U;
    fill(actuator.body.sim_actuator.motor, 0.1f);
    const mark4_SimActuator &backActuator = roundTrip(actuator, size).body.sim_actuator;
    CHECK(backActuator.echo_timestamp_us == 4'000U);
    CHECK(same(backActuator.motor, actuator.body.sim_actuator.motor));

    mark4_Envelope scenario = withBody(mark4_Envelope_sim_scenario_tag);
    mark4_SimScenario &run = scenario.body.sim_scenario;
    run.sequence = 7U;
    run.kind = mark4_SimScenarioKind_HAND_THROW;
    run.seed = 0x0123456789ABCDEFULL;
    run.throw_delay_us = 2'000'000U;
    run.hash_window_us = 16'000'000U;
    fill(run.velocity_mps, 1.0f);
    fill(run.angular_velocity_rad_s, 4.0f);
    run.held_seconds = 1.5f;
    run.held_tilt_rad = 0.25f;
    run.held_azimuth_rad = 0.5f;
    run.swing_seconds = 0.375f;
    const mark4_SimScenario &backRun = roundTrip(scenario, size).body.sim_scenario;
    CHECK(backRun.sequence == 7U);
    CHECK(backRun.kind == mark4_SimScenarioKind_HAND_THROW);
    CHECK(backRun.seed == 0x0123456789ABCDEFULL);
    CHECK(backRun.throw_delay_us == 2'000'000U);
    CHECK(backRun.hash_window_us == 16'000'000U);
    CHECK(same(backRun.velocity_mps, run.velocity_mps));
    CHECK(same(backRun.angular_velocity_rad_s, run.angular_velocity_rad_s));
    CHECK(backRun.held_seconds == 1.5f);
    CHECK(backRun.held_tilt_rad == 0.25f);
    CHECK(backRun.held_azimuth_rad == 0.5f);
    CHECK(backRun.swing_seconds == 0.375f);

    mark4_Envelope stats = withBody(mark4_Envelope_sim_run_stats_tag);
    stats.body.sim_run_stats.run_id = 42U;
    stats.body.sim_run_stats.final = true;
    stats.body.sim_run_stats.degraded = true;
    stats.body.sim_run_stats.run_start_us = 1'000'000U;
    stats.body.sim_run_stats.run_hash = 0xFEEDFACECAFEBEEFULL;
    stats.body.sim_run_stats.duplicate_frames = 3U;
    stats.body.sim_run_stats.lockstep_timeouts = 7U;
    const mark4_SimRunStats &backStats = roundTrip(stats, size).body.sim_run_stats;
    CHECK(backStats.run_id == 42U);
    CHECK(backStats.final);
    CHECK(backStats.degraded);
    CHECK(backStats.run_start_us == 1'000'000U);
    CHECK(backStats.run_hash == 0xFEEDFACECAFEBEEFULL);
    CHECK(backStats.duplicate_frames == 3U);
    CHECK(backStats.lockstep_timeouts == 7U);
}

TEST_CASE("the command and identity messages round trip")
{
    std::size_t size = 0U;

    mark4_Envelope rc = withBody(mark4_Envelope_rc_tag);
    rc.body.rc.kill = false;
    rc.body.rc.arm = true;
    rc.body.rc.mode = mark4_RcMode_RC_ALTITUDE_AUTO;
    rc.body.rc.throttle = 0.5f;
    const mark4_Rc &backRc = roundTrip(rc, size).body.rc;
    CHECK(!backRc.kill);
    CHECK(backRc.arm);
    CHECK(backRc.mode == mark4_RcMode_RC_ALTITUDE_AUTO);
    CHECK(backRc.throttle == 0.5f);

    mark4_Envelope announce = withBody(mark4_Envelope_announce_tag);
    announce.body.announce.kind = mark4_NodeKind_FIRMWARE;
    static_cast<void>(std::snprintf(
        announce.body.announce.name, sizeof(announce.body.announce.name), "%s", "mark4-fc"));
    announce.body.announce.mcu = mark4_Mcu_STM32F405;
    announce.body.announce.build_epoch = 0x66E00001U;
    static_cast<void>(std::snprintf(announce.body.announce.git_hash,
                                    sizeof(announce.body.announce.git_hash),
                                    "%s",
                                    "deadbeef"));
    announce.body.announce.wire_hash = mark4::WIRE_HASH;
    const mark4_Announce &backAnnounce = roundTrip(announce, size).body.announce;
    CHECK(backAnnounce.kind == mark4_NodeKind_FIRMWARE);
    CHECK(std::string(backAnnounce.name) == "mark4-fc");
    CHECK(backAnnounce.mcu == mark4_Mcu_STM32F405);
    CHECK(backAnnounce.build_epoch == 0x66E00001U);
    CHECK(std::string(backAnnounce.git_hash) == "deadbeef");
    CHECK(backAnnounce.wire_hash == mark4::WIRE_HASH);

    mark4_Envelope log = withBody(mark4_Envelope_log_tag);
    log.body.log.timestamp_us = 12U;
    log.body.log.level = mark4_LogLevel_WARN;
    log.body.log.module_id = 17U;
    static_cast<void>(
        std::snprintf(log.body.log.text, sizeof(log.body.log.text), "%s", "init failed"));
    const mark4_Log &backLog = roundTrip(log, size).body.log;
    CHECK(backLog.timestamp_us == 12U);
    CHECK(backLog.level == mark4_LogLevel_WARN);
    CHECK(backLog.module_id == 17U);
    CHECK(std::string(backLog.text) == "init failed");

    mark4_Envelope modules = withBody(mark4_Envelope_log_modules_tag);
    modules.body.log_modules.start_index = 8U;
    modules.body.log_modules.total = 9U;
    modules.body.log_modules.modules_count = 1U;
    modules.body.log_modules.modules[0].id = 17U;
    modules.body.log_modules.modules[0].level = mark4_LogLevel_TRACE;
    static_cast<void>(std::snprintf(modules.body.log_modules.modules[0].name,
                                    sizeof(modules.body.log_modules.modules[0].name),
                                    "%s",
                                    "platform/baro"));
    const mark4_LogModules &backModules = roundTrip(modules, size).body.log_modules;
    CHECK(backModules.start_index == 8U);
    CHECK(backModules.total == 9U);
    REQUIRE(backModules.modules_count == 1U);
    CHECK(backModules.modules[0].id == 17U);
    CHECK(backModules.modules[0].level == mark4_LogLevel_TRACE);
    CHECK(std::string(backModules.modules[0].name) == "platform/baro");

    mark4_Envelope control = withBody(mark4_Envelope_log_control_tag);
    control.body.log_control.which_request = mark4_LogControl_set_tag;
    control.body.log_control.request.set.module_id = 17U;
    control.body.log_control.request.set.level = mark4_LogLevel_DEBUG;
    const mark4_LogControl &backControl = roundTrip(control, size).body.log_control;
    REQUIRE(backControl.which_request == mark4_LogControl_set_tag);
    CHECK(backControl.request.set.module_id == 17U);
    CHECK(backControl.request.set.level == mark4_LogLevel_DEBUG);
    control.body.log_control.which_request = mark4_LogControl_query_tag;
    control.body.log_control.request.query = true;
    CHECK(roundTrip(control, size).body.log_control.which_request == mark4_LogControl_query_tag);

    // The bodies with no field at all still name themselves on the wire.
    CHECK(roundTrip(withBody(mark4_Envelope_reboot_tag), size).which_body ==
          mark4_Envelope_reboot_tag);
    CHECK(size <= 3U); // a two-byte tag and an empty length
    CHECK(roundTrip(withBody(mark4_Envelope_ota_status_request_tag), size).which_body ==
          mark4_Envelope_ota_status_request_tag);
    CHECK(roundTrip(withBody(mark4_Envelope_ota_revert_tag), size).which_body ==
          mark4_Envelope_ota_revert_tag);
}

TEST_CASE("the tuning messages round trip")
{
    std::size_t size = 0U;

    mark4_Envelope set = withBody(mark4_Envelope_tuning_set_tag);
    set.body.tuning_set.id = 101U;
    set.body.tuning_set.value = 0.028f;
    CHECK(roundTrip(set, size).body.tuning_set.id == 101U);
    CHECK(roundTrip(set, size).body.tuning_set.value == 0.028f);

    mark4_Envelope get = withBody(mark4_Envelope_tuning_get_tag);
    get.body.tuning_get.id = 102U;
    CHECK(roundTrip(get, size).body.tuning_get.id == 102U);

    mark4_Envelope list = withBody(mark4_Envelope_tuning_list_tag);
    list.body.tuning_list.start_index = 4U;
    CHECK(roundTrip(list, size).body.tuning_list.start_index == 4U);

    mark4_Envelope ack = withBody(mark4_Envelope_tuning_ack_tag);
    ack.body.tuning_ack.id = 101U;
    ack.body.tuning_ack.value = 0.028f;
    ack.body.tuning_ack.status = mark4_TuningStatus_LOCKED_WHILE_ARMED;
    const mark4_TuningAck &backAck = roundTrip(ack, size).body.tuning_ack;
    CHECK(backAck.id == 101U);
    CHECK(backAck.value == 0.028f);
    CHECK(backAck.status == mark4_TuningStatus_LOCKED_WHILE_ARMED);

    mark4_Envelope info = withBody(mark4_Envelope_tuning_info_tag);
    info.body.tuning_info.index = 3U;
    info.body.tuning_info.count = 12U;
    info.body.tuning_info.id = 401U;
    // A name filling the whole field: 16 characters plus the terminator.
    static_cast<void>(std::snprintf(
        info.body.tuning_info.name, sizeof(info.body.tuning_info.name), "%s", "abcdefghijklmnop"));
    info.body.tuning_info.value = 2.0f;
    info.body.tuning_info.min_value = 0.5f;
    info.body.tuning_info.max_value = 8.0f;
    info.body.tuning_info.armed_change = true;
    const mark4_TuningInfo &backInfo = roundTrip(info, size).body.tuning_info;
    CHECK(backInfo.index == 3U);
    CHECK(backInfo.count == 12U);
    CHECK(backInfo.id == 401U);
    CHECK(std::string(backInfo.name) == "abcdefghijklmnop");
    CHECK(backInfo.value == 2.0f);
    CHECK(backInfo.min_value == 0.5f);
    CHECK(backInfo.max_value == 8.0f);
    CHECK(backInfo.armed_change);
}

TEST_CASE("the updater messages round trip, the chunk at its full size")
{
    std::size_t size = 0U;

    mark4_Envelope status = withBody(mark4_Envelope_ota_status_tag);
    status.body.ota_status.mcu = mark4_Mcu_STM32F405;
    status.body.ota_status.running_slot = 1U;
    status.body.ota_status.active_slot = 0U;
    status.body.ota_status.updater_busy = true;
    status.body.ota_status.slots[0].state = mark4_OtaSlotState_VALID;
    status.body.ota_status.slots[0].build_epoch = 0x66E00001U;
    static_cast<void>(std::snprintf(status.body.ota_status.slots[0].git_hash,
                                    sizeof(status.body.ota_status.slots[0].git_hash),
                                    "%s",
                                    "aaaaaaaa"));
    status.body.ota_status.slots[1].state = mark4_OtaSlotState_TESTING;
    status.body.ota_status.slots[1].build_epoch = 0xFFFFFFFFU;
    status.body.ota_status.slot_size = 393216U;
    status.body.ota_status.max_chunk_data = 240U;
    const mark4_OtaStatus &backStatus = roundTrip(status, size).body.ota_status;
    CHECK(backStatus.mcu == mark4_Mcu_STM32F405);
    CHECK(backStatus.running_slot == 1U);
    CHECK(backStatus.active_slot == 0U);
    CHECK(backStatus.updater_busy);
    CHECK(backStatus.slots[0].state == mark4_OtaSlotState_VALID);
    CHECK(backStatus.slots[0].build_epoch == 0x66E00001U);
    CHECK(std::string(backStatus.slots[0].git_hash) == "aaaaaaaa");
    CHECK(backStatus.slots[1].state == mark4_OtaSlotState_TESTING);
    CHECK(backStatus.slots[1].build_epoch == 0xFFFFFFFFU);
    CHECK(std::string(backStatus.slots[1].git_hash).empty());
    CHECK(backStatus.slot_size == 393216U);
    CHECK(backStatus.max_chunk_data == 240U);

    mark4_Envelope begin = withBody(mark4_Envelope_ota_begin_tag);
    begin.body.ota_begin.session = 0xA5A5F00DU;
    begin.body.ota_begin.image_size = 4608U;
    begin.body.ota_begin.image_crc = 0xC704DD7BU;
    const mark4_OtaBegin &backBegin = roundTrip(begin, size).body.ota_begin;
    CHECK(backBegin.session == 0xA5A5F00DU);
    CHECK(backBegin.image_size == 4608U);
    CHECK(backBegin.image_crc == 0xC704DD7BU);

    mark4_Envelope chunk = withBody(mark4_Envelope_ota_chunk_tag);
    chunk.body.ota_chunk.session = 0xA5A5F00DU;
    chunk.body.ota_chunk.offset = 240U;
    chunk.body.ota_chunk.data.size = sizeof(chunk.body.ota_chunk.data.bytes);
    for (std::size_t index = 0U; index < chunk.body.ota_chunk.data.size; ++index)
    {
        chunk.body.ota_chunk.data.bytes[index] = static_cast<std::uint8_t>(index * 31U);
    }
    const mark4_OtaChunk &backChunk = roundTrip(chunk, size).body.ota_chunk;
    CHECK(backChunk.session == 0xA5A5F00DU);
    CHECK(backChunk.offset == 240U);
    REQUIRE(backChunk.data.size == chunk.body.ota_chunk.data.size);
    CHECK(std::memcmp(backChunk.data.bytes, chunk.body.ota_chunk.data.bytes, backChunk.data.size) ==
          0);
    // The full chunk was the largest message of the wire until a LogModules
    // page took over; it still has to carry its 240 bytes.
    CHECK(size > 240U);

    mark4_Envelope chunkAck = withBody(mark4_Envelope_ota_chunk_ack_tag);
    chunkAck.body.ota_chunk_ack.session = 1U;
    chunkAck.body.ota_chunk_ack.next_offset = 3840U;
    CHECK(roundTrip(chunkAck, size).body.ota_chunk_ack.next_offset == 3840U);

    mark4_Envelope finish = withBody(mark4_Envelope_ota_finish_tag);
    finish.body.ota_finish.session = 2U;
    CHECK(roundTrip(finish, size).body.ota_finish.session == 2U);

    mark4_Envelope abort = withBody(mark4_Envelope_ota_abort_tag);
    abort.body.ota_abort.session = 3U;
    CHECK(roundTrip(abort, size).body.ota_abort.session == 3U);

    mark4_Envelope ack = withBody(mark4_Envelope_ota_ack_tag);
    ack.body.ota_ack.session = 4U;
    ack.body.ota_ack.op = mark4_OtaOp_FINISH;
    ack.body.ota_ack.result = mark4_OtaResult_CRC_MISMATCH;
    const mark4_OtaAck &backAck = roundTrip(ack, size).body.ota_ack;
    CHECK(backAck.session == 4U);
    CHECK(backAck.op == mark4_OtaOp_FINISH);
    CHECK(backAck.result == mark4_OtaResult_CRC_MISMATCH);
}

TEST_CASE("the codec refuses what is not an envelope")
{
    std::array<std::uint8_t, 16U> bytes{};
    std::size_t size = 0U;
    mark4_Envelope decoded;

    // Nothing to encode: an envelope with no body is a bug, not a message.
    CHECK(!mark4::encodeEnvelope(mark4_Envelope_init_zero, bytes.data(), bytes.size(), size));

    // Too small a buffer.
    mark4_Envelope chunk = withBody(mark4_Envelope_ota_chunk_tag);
    chunk.body.ota_chunk.data.size = 100U;
    CHECK(!mark4::encodeEnvelope(chunk, bytes.data(), bytes.size(), size));

    // Garbage in.
    const std::array<std::uint8_t, 4> garbage = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    CHECK(!mark4::decodeEnvelope(garbage.data(), garbage.size(), decoded));
    CHECK(!mark4::decodeEnvelope(nullptr, 0U, decoded));
    CHECK(!mark4::decodeEnvelope(garbage.data(), 0U, decoded));

    // A valid message cut short.
    mark4_Envelope rc = withBody(mark4_Envelope_rc_tag);
    rc.body.rc.throttle = 0.5f;
    REQUIRE(mark4::encodeEnvelope(rc, bytes.data(), bytes.size(), size));
    CHECK(!mark4::decodeEnvelope(bytes.data(), size - 1U, decoded));
}
