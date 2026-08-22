/// @file
/// @brief Golden packet generator and checker. Emits reference bytes with
///        asymmetric field values for every packet type of the protocol
///        (plus the stored formats that cross a process boundary: the
///        blackbox record and the OTA image header);
///        the fixtures are committed, a ctest re-generates and compares
///        them (C++ drift guard), and the python and GDScript decode
///        tests read the same files, so every hand-written parser is
///        checked against the exact bytes the C++ structs produce.
///
///        Usage: golden_packets --check <fixtures dir>
///               golden_packets --write <fixtures dir>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform_common/crc32_mpeg2.hpp"
#include "protocol/announce.hpp"
#include "protocol/blackbox.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"
#include "protocol/ports.hpp"
#include "protocol/serial_framing.hpp"
#include "protocol/sim_link.hpp"
#include "protocol/sim_raw.hpp"
#include "protocol/sim_stats.hpp"
#include "protocol/telemetry.hpp"
#include "protocol/tuning.hpp"

namespace
{
    /// One reference file: its basename and the exact bytes it must hold.
    struct Fixture
    {
        std::string name;                ///< file name inside the fixtures dir
        std::vector<std::uint8_t> bytes; ///< reference content
    };

    /// @brief Serializes a packed struct, then patches the float arrays in
    ///        with memcpy (they sit at unaligned offsets by design).
    template <typename Packet> std::vector<std::uint8_t> toBytes(const Packet &packet)
    {
        std::vector<std::uint8_t> bytes(sizeof(Packet));
        std::memcpy(bytes.data(), &packet, sizeof(Packet));
        return bytes;
    }

    /// @brief Writes a float array into packet bytes at the given offset.
    template <std::size_t N>
    void patch(std::vector<std::uint8_t> &bytes,
               std::size_t offset,
               const std::array<float, N> &values)
    {
        std::memcpy(bytes.data() + offset, values.data(), sizeof(values));
    }

    std::vector<std::uint8_t> makeTelemetry()
    {
        mark4::TelemetryPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TELEMETRY);
        packet.sourceId = static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM);
        packet.sequence = 48879U; // 0xBEEF
        packet.timestampUs = 1234567890123456789ULL;
        packet.altitudeM = 12.5f;
        packet.verticalVelocityMps = -3.5f;
        packet.throwState = 2U;
        packet.throwCount = 7U;
        packet.releaseVelocityMps = 4.75f;
        packet.apexTimestampUs = 987654321ULL;
        packet.apexAltitudeM = 6.25f;
        packet.flightPhase = 5U;
        auto bytes = toBytes(packet);
        patch(bytes,
              offsetof(mark4::TelemetryPacket, gyroRadS),
              std::array<float, 3>{1.5f, -2.25f, 3.125f});
        patch(bytes,
              offsetof(mark4::TelemetryPacket, attitudeQuat),
              std::array<float, 4>{0.5f, -0.25f, 0.125f, -0.0625f});
        patch(bytes,
              offsetof(mark4::TelemetryPacket, gyroBiasRadS),
              std::array<float, 3>{0.03125f, -0.0625f, 0.09375f});
        patch(bytes,
              offsetof(mark4::TelemetryPacket, motor),
              std::array<float, 4>{0.125f, 0.25f, 0.375f, 0.5f});
        return bytes;
    }

    std::vector<std::uint8_t> makeSimRaw()
    {
        mark4::SimRawPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_RAW);
        packet.sourceId = static_cast<std::uint8_t>(mark4::StreamSource::SIM_PLANT);
        packet.sequence = 513U; // 0x0201
        packet.timestampUs = 111222333ULL;
        auto bytes = toBytes(packet);
        patch(bytes,
              offsetof(mark4::SimRawPacket, attitudeQuat),
              std::array<float, 4>{0.875f, -0.375f, 0.1875f, -0.09375f});
        patch(bytes,
              offsetof(mark4::SimRawPacket, positionM),
              std::array<float, 3>{1.5f, -2.5f, 3.5f});
        patch(bytes,
              offsetof(mark4::SimRawPacket, velocityMps),
              std::array<float, 3>{-0.5f, 0.75f, -1.25f});
        return bytes;
    }

    std::vector<std::uint8_t> makeSimSensor()
    {
        mark4::SimSensorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_SENSOR);
        packet.timestampUs = 555666777ULL;
        packet.baroPa = 101325.0f;
        packet.resetCount = 9U;
        packet.sessionId = 3735928559U;  // 0xDEADBEEF
        packet.lockstepTimeouts = 4660U; // 0x1234
        auto bytes = toBytes(packet);
        patch(bytes,
              offsetof(mark4::SimSensorPacket, gyroRadS),
              std::array<float, 3>{0.5f, -1.5f, 2.5f});
        patch(bytes,
              offsetof(mark4::SimSensorPacket, accelMps2),
              std::array<float, 3>{0.25f, -0.75f, 9.5f});
        return bytes;
    }

    /// @brief The reference scenario block, the same bytes wherever it
    ///        rides: inside SimScenarioPacket and inside the lockstep reply.
    /// @return the block, every field distinctive
    mark4::SimScenario makeScenarioBlock()
    {
        mark4::SimScenario scenario{};
        scenario.sequence = 200U;
        scenario.scenario = mark4::SIM_SCENARIO_HAND_THROW;
        scenario.seed = 0x0123456789ABCDEFULL;
        scenario.throwDelayUs = 2000000U;
        scenario.hashWindowUs = 16000000U;
        scenario.heldSeconds = 1.5f;
        scenario.heldTiltRad = 0.75f;
        scenario.heldAzimuthRad = -2.5f;
        scenario.swingSeconds = 0.3125f;
        return scenario;
    }

    /// @brief Writes the float arrays of a scenario block into packet bytes.
    /// @param bytes packet bytes being built
    /// @param blockOffset offset the block starts at
    void patchScenarioVectors(std::vector<std::uint8_t> &bytes, std::size_t blockOffset)
    {
        patch(bytes,
              blockOffset + offsetof(mark4::SimScenario, velocityMps),
              std::array<float, 3>{1.25f, -2.5f, 5.5f});
        patch(bytes,
              blockOffset + offsetof(mark4::SimScenario, angularVelocityRadS),
              std::array<float, 3>{-0.5f, 1.5f, -2.25f});
    }

    std::vector<std::uint8_t> makeSimActuator()
    {
        mark4::SimActuatorPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_ACTUATOR);
        packet.echoTimestampUs = 555666777ULL;
        packet.scenario = makeScenarioBlock();
        auto bytes = toBytes(packet);
        patch(bytes,
              offsetof(mark4::SimActuatorPacket, motor),
              std::array<float, 4>{0.0625f, 0.125f, 0.1875f, 0.25f});
        patchScenarioVectors(bytes, offsetof(mark4::SimActuatorPacket, scenario));
        return bytes;
    }

    std::vector<std::uint8_t> makeSimScenario()
    {
        mark4::SimScenarioPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_SCENARIO);
        packet.scenario = makeScenarioBlock();
        auto bytes = toBytes(packet);
        patchScenarioVectors(bytes, offsetof(mark4::SimScenarioPacket, scenario));
        return bytes;
    }

    std::vector<std::uint8_t> makeSimRunStats()
    {
        mark4::SimRunStatsPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::SIM_RUN_STATS);
        packet.sourceId = static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM);
        packet.sequence = 4919U; // 0x1337
        packet.runId = 23U;
        packet.flags = mark4::SIM_RUN_FLAG_LOCKSTEP_DEGRADED | mark4::SIM_RUN_FLAG_HASH_SEALED;
        packet.runStartUs = 24680135ULL;
        packet.runHash = 0xFEDCBA9876543210ULL;
        packet.duplicateFrames = 11U;
        packet.lockstepTimeouts = 2U;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeRcCommand()
    {
        mark4::RcCommandPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND);
        packet.killSwitch = 0U;
        packet.armSwitch = 1U;
        packet.mode = mark4::RC_MODE_ALTITUDE_AUTO;
        packet.throttle = 0.8125f;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeReboot()
    {
        mark4::RebootCommandPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::REBOOT_COMMAND);
        packet.magic = mark4::BOARD_REBOOT_MAGIC;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeAnnounce()
    {
        mark4::AnnouncePacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::ANNOUNCE);
        packet.kind = static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM);
        packet.sessionId = 3405691582U; // 0xCAFEBABE
        packet.telemetryPort = mark4::TELEMETRY_PORT;
        packet.commandPort = mark4::RC_COMMAND_PORT;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeTuningSet()
    {
        mark4::TuningSetPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_SET);
        packet.id = 258U; // 0x0102
        packet.value = 2.5f;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeTuningGet()
    {
        mark4::TuningGetPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_GET);
        packet.id = 772U; // 0x0304
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeTuningList()
    {
        mark4::TuningListPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_LIST);
        packet.startIndex = 5U;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeTuningAck()
    {
        mark4::TuningAckPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_ACK);
        packet.id = 258U;
        packet.value = -1.5f;
        packet.status = mark4::TUNING_ACK_OUT_OF_BOUNDS;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeTuningInfo()
    {
        mark4::TuningInfoPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::TUNING_INFO);
        packet.index = 3U;
        packet.count = 17U;
        packet.id = 258U;
        const char name[] = "rate_kp";
        std::memcpy(packet.name.data(), name, sizeof(name));
        packet.value = 0.09375f;
        packet.minValue = 0.0f;
        packet.maxValue = 1.5f;
        packet.flags = mark4::TUNING_FLAG_ARMED_CHANGE;
        return toBytes(packet);
    }

    /// The session nonce every packet of one update session echoes; its
    /// four bytes differ so a byte-order slip cannot go unnoticed.
    constexpr std::uint32_t OTA_FIXTURE_SESSION = 0x1A2B3C4DU;

    /// Offset of the fixture chunk: chunk 1146 of a transfer, so it is a
    /// whole number of full chunks into the image.
    constexpr std::uint32_t OTA_FIXTURE_CHUNK_OFFSET = 275040U;

    /// Valid data bytes of the fixture chunk: deliberately fewer than
    /// OTA_CHUNK_DATA_SIZE, so a decoder that ignores length is caught.
    constexpr std::uint8_t OTA_FIXTURE_CHUNK_LENGTH = 5U;

    /// Total image bytes announced by the begin and stamped in the header.
    constexpr std::uint32_t OTA_FIXTURE_IMAGE_SIZE = 0x00021308U;

    std::vector<std::uint8_t> makeOtaStatusRequest()
    {
        mark4::OtaStatusRequestPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_STATUS_REQUEST);
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaStatus()
    {
        mark4::OtaStatusPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_STATUS);
        packet.mcuId = mark4::OTA_MCU_STM32F405;
        // A trial boot of slot B, the previous image still valid in A and
        // still the active one until the trial confirms.
        packet.runningSlot = mark4::OTA_SLOT_B;
        packet.activeSlot = mark4::OTA_SLOT_A;
        packet.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};
        packet.updaterBusy = 0U;
        packet.versionMajor = 1U;
        packet.versionMinor = 2U;
        packet.versionPatch = 3U;
        std::memcpy(packet.gitHash.data(), "deadbeef", packet.gitHash.size());
        packet.slotSize = 0x00060102U;
        packet.maxChunkData = static_cast<std::uint16_t>(mark4::OTA_CHUNK_DATA_SIZE);
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaBegin()
    {
        mark4::OtaBeginPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_BEGIN);
        packet.session = OTA_FIXTURE_SESSION;
        packet.imageSize = OTA_FIXTURE_IMAGE_SIZE;
        packet.imageCrc = 0x89ABCDEFU;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaChunk()
    {
        mark4::OtaChunkPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_CHUNK);
        packet.session = OTA_FIXTURE_SESSION;
        packet.offset = OTA_FIXTURE_CHUNK_OFFSET;
        packet.length = OTA_FIXTURE_CHUNK_LENGTH;
        packet.data = {};
        packet.data[0] = 0xDEU;
        packet.data[1] = 0xADU;
        packet.data[2] = 0xBEU;
        packet.data[3] = 0xEFU;
        packet.data[4] = 0x2AU;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaChunkAck()
    {
        mark4::OtaChunkAckPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_CHUNK_ACK);
        packet.session = OTA_FIXTURE_SESSION;
        // Cumulative: the chunk above landed, the next byte is expected.
        packet.nextOffset = OTA_FIXTURE_CHUNK_OFFSET + OTA_FIXTURE_CHUNK_LENGTH;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaFinish()
    {
        mark4::OtaFinishPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_FINISH);
        packet.session = OTA_FIXTURE_SESSION;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaConfirm()
    {
        mark4::OtaConfirmPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_CONFIRM);
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaRevert()
    {
        mark4::OtaRevertPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_REVERT);
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaAbort()
    {
        mark4::OtaAbortPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_ABORT);
        packet.session = OTA_FIXTURE_SESSION;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaAck()
    {
        mark4::OtaAckPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_ACK);
        packet.session = OTA_FIXTURE_SESSION;
        // A refusal, so both bytes after the session are distinctive.
        packet.ackedType = static_cast<std::uint8_t>(mark4::PacketType::OTA_FINISH);
        packet.result = mark4::OTA_RESULT_CRC_MISMATCH;
        return toBytes(packet);
    }

    std::vector<std::uint8_t> makeOtaImageHeader()
    {
        mark4::OtaImageHeader header{};
        header.magic = mark4::OTA_IMAGE_MAGIC;
        header.headerVersion = mark4::OTA_IMAGE_HEADER_VERSION;
        header.mcuId = mark4::OTA_MCU_STM32F722;
        header.slotId = mark4::OTA_SLOT_A;
        header.imageSize = OTA_FIXTURE_IMAGE_SIZE;
        header.imageCrc = 0x76543210U;
        header.versionMajor = 4U;
        header.versionMinor = 5U;
        header.versionPatch = 6U;
        header.reserved0 = 0xFFU;
        std::memcpy(header.gitHash.data(), "0badc0de", header.gitHash.size());
        header.reserved.fill(0xFFU);
        auto bytes = toBytes(header);
        // Stamped last, over everything above it, exactly as the packaging
        // script does: the python side recomputes it from these bytes.
        const std::uint32_t crc =
            mark4::crc32Mpeg2(bytes.data(), offsetof(mark4::OtaImageHeader, headerCrc));
        std::memcpy(bytes.data() + offsetof(mark4::OtaImageHeader, headerCrc), &crc, sizeof(crc));
        return bytes;
    }

    std::vector<std::uint8_t> makeBlackboxRecord()
    {
        mark4::BlackboxRecord record{};
        record.sync0 = mark4::BLACKBOX_SYNC0;
        record.sync1 = mark4::BLACKBOX_SYNC1;
        record.version = mark4::PROTOCOL_VERSION;
        record.type = static_cast<std::uint8_t>(mark4::PacketType::BLACKBOX_RECORD);
        record.length = mark4::BLACKBOX_RECORD_PAYLOAD_SIZE;
        record.timestampUs = 987654321ULL;
        record.baroPa = 101325.0f;
        record.killSwitch = 0U;
        record.throttle = 0.75f;
        record.armSwitch = 1U;
        auto bytes = toBytes(record);
        patch(bytes,
              offsetof(mark4::BlackboxRecord, gyroRadS),
              std::array<float, 3>{0.25f, -0.5f, 1.5f});
        patch(bytes,
              offsetof(mark4::BlackboxRecord, accelMps2),
              std::array<float, 3>{0.0f, 0.0f, 9.80665f});
        patch(bytes,
              offsetof(mark4::BlackboxRecord, motor),
              std::array<float, 4>{0.125f, 0.25f, 0.5f, 0.75f});
        const std::uint16_t crc = mark4::blackboxRecordCrc(bytes.data());
        bytes[mark4::BLACKBOX_CRC_OFFSET] = static_cast<std::uint8_t>(crc & mark4::CRC16_LOW_MASK);
        bytes[mark4::BLACKBOX_CRC_OFFSET + 1U] =
            static_cast<std::uint8_t>(crc >> mark4::CRC16_BYTE_SHIFT);
        return bytes;
    }

    std::vector<std::uint8_t> makeSerialFrame()
    {
        const std::array<std::uint8_t, 5> payload = {0x01U, 0x02U, 0x03U, 0x04U, 0xFAU};
        std::vector<std::uint8_t> frame(payload.size() + mark4::SERIAL_FRAME_OVERHEAD);
        const std::size_t written =
            mark4::encodeSerialFrame(payload.data(), payload.size(), frame.data());
        frame.resize(written);
        return frame;
    }

    std::vector<Fixture> buildFixtures()
    {
        return {
            {"telemetry.bin", makeTelemetry()},
            {"sim_raw.bin", makeSimRaw()},
            {"sim_sensor.bin", makeSimSensor()},
            {"sim_actuator.bin", makeSimActuator()},
            {"sim_scenario.bin", makeSimScenario()},
            {"sim_run_stats.bin", makeSimRunStats()},
            {"rc_command.bin", makeRcCommand()},
            {"reboot_command.bin", makeReboot()},
            {"announce.bin", makeAnnounce()},
            {"tuning_set.bin", makeTuningSet()},
            {"tuning_get.bin", makeTuningGet()},
            {"tuning_list.bin", makeTuningList()},
            {"tuning_ack.bin", makeTuningAck()},
            {"tuning_info.bin", makeTuningInfo()},
            {"ota_status_request.bin", makeOtaStatusRequest()},
            {"ota_status.bin", makeOtaStatus()},
            {"ota_begin.bin", makeOtaBegin()},
            {"ota_chunk.bin", makeOtaChunk()},
            {"ota_chunk_ack.bin", makeOtaChunkAck()},
            {"ota_finish.bin", makeOtaFinish()},
            {"ota_confirm.bin", makeOtaConfirm()},
            {"ota_revert.bin", makeOtaRevert()},
            {"ota_abort.bin", makeOtaAbort()},
            {"ota_ack.bin", makeOtaAck()},
            {"ota_image_header.bin", makeOtaImageHeader()},
            {"blackbox_record.bin", makeBlackboxRecord()},
            {"serial_frame.bin", makeSerialFrame()},
        };
    }

    /// @brief Writes every fixture into the directory.
    /// @return process exit code
    int writeFixtures(const std::string &directory)
    {
        for (const Fixture &fixture : buildFixtures())
        {
            const std::string path = directory + "/" + fixture.name;
            std::FILE *file = std::fopen(path.c_str(), "wb");
            if (file == nullptr)
            {
                static_cast<void>(
                    std::fprintf(stderr, "golden_packets: cannot write %s\n", path.c_str()));
                return 1;
            }
            const std::size_t written =
                std::fwrite(fixture.bytes.data(), 1U, fixture.bytes.size(), file);
            static_cast<void>(std::fclose(file));
            if (written != fixture.bytes.size())
            {
                static_cast<void>(
                    std::fprintf(stderr, "golden_packets: short write on %s\n", path.c_str()));
                return 1;
            }
            static_cast<void>(
                std::printf("wrote %s (%zu bytes)\n", path.c_str(), fixture.bytes.size()));
        }
        return 0;
    }

    /// @brief Compares every fixture against the committed files.
    /// @return process exit code, nonzero on any mismatch
    int checkFixtures(const std::string &directory)
    {
        int failures = 0;
        for (const Fixture &fixture : buildFixtures())
        {
            const std::string path = directory + "/" + fixture.name;
            std::FILE *file = std::fopen(path.c_str(), "rb");
            if (file == nullptr)
            {
                static_cast<void>(
                    std::fprintf(stderr, "golden_packets: missing fixture %s\n", path.c_str()));
                ++failures;
                continue;
            }
            std::vector<std::uint8_t> disk(fixture.bytes.size() + 1U);
            const std::size_t read = std::fread(disk.data(), 1U, disk.size(), file);
            static_cast<void>(std::fclose(file));
            disk.resize(read);
            if (disk != fixture.bytes)
            {
                static_cast<void>(
                    std::fprintf(stderr,
                                 "golden_packets: %s differs from the generated bytes "
                                 "(committed %zu bytes, generated %zu)\n",
                                 path.c_str(),
                                 disk.size(),
                                 fixture.bytes.size()));
                ++failures;
            }
        }
        if (failures == 0)
        {
            static_cast<void>(
                std::printf("golden_packets: %zu fixtures match\n", buildFixtures().size()));
        }
        return failures == 0 ? 0 : 1;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        static_cast<void>(
            std::fprintf(stderr, "usage: golden_packets --check|--write <fixtures dir>\n"));
        return 2;
    }
    const std::string mode = argv[1];
    const std::string directory = argv[2];
    if (mode == "--write")
    {
        return writeFixtures(directory);
    }
    if (mode == "--check")
    {
        return checkFixtures(directory);
    }
    static_cast<void>(std::fprintf(stderr, "golden_packets: unknown mode '%s'\n", mode.c_str()));
    return 2;
}
