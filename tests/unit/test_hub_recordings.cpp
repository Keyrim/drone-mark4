/// @file
/// @brief Reading the log directory back: what it holds, and what one
///        recording decodes to once it has been thinned down to something a
///        browser can draw.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hub/recordings.hpp"
#include "hub/stream_recorder.hpp"
#include "protocol/blackbox.hpp"

namespace
{
    /// @brief Private directory for one test, emptied beforehand.
    /// @param name directory name
    /// @return the path
    std::string scratchDirectory(const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::error_code failure;
        std::filesystem::remove_all(path, failure);
        std::filesystem::create_directories(path, failure);
        return path.string();
    }

    /// @brief Builds one valid blackbox record.
    /// @param timestampUs acquisition time to carry [us]
    /// @return the serialized record bytes
    std::array<std::uint8_t, mark4::BLACKBOX_RECORD_SIZE> blackboxRecord(std::uint64_t timestampUs)
    {
        mark4::BlackboxRecord record;
        record.sync0 = mark4::BLACKBOX_SYNC0;
        record.sync1 = mark4::BLACKBOX_SYNC1;
        record.version = mark4::PROTOCOL_VERSION;
        record.type = static_cast<std::uint8_t>(mark4::PacketType::BLACKBOX_RECORD);
        record.length = mark4::BLACKBOX_RECORD_PAYLOAD_SIZE;
        record.timestampUs = timestampUs;
        record.gyroRadS = {0.25f, -0.5f, 0.75f};
        record.accelMps2 = {0.0f, 0.0f, 9.80665f};
        record.baroPa = 101325.0f;
        record.killSwitch = 1U;
        record.throttle = 0.5f;
        record.armSwitch = 1U;
        record.motor = {0.1f, 0.2f, 0.3f, 0.4f};

        std::array<std::uint8_t, mark4::BLACKBOX_RECORD_SIZE> bytes{};
        std::memcpy(bytes.data(), &record, bytes.size());
        const std::uint16_t crc = mark4::blackboxRecordCrc(bytes.data());
        bytes[mark4::BLACKBOX_CRC_OFFSET] = static_cast<std::uint8_t>(crc & mark4::CRC16_LOW_MASK);
        bytes[mark4::BLACKBOX_CRC_OFFSET + 1U] =
            static_cast<std::uint8_t>(crc >> mark4::CRC16_BYTE_SHIFT);
        return bytes;
    }

    /// @brief Writes one blackbox file.
    /// @param path file to write
    /// @param records number of records to put in it
    /// @param garbage bytes of junk to slip between the first two
    void writeBlackbox(const std::string &path, std::size_t records, std::size_t garbage)
    {
        std::ofstream file(path, std::ios::binary);
        for (std::size_t index = 0U; index < records; ++index)
        {
            if (index == 1U)
            {
                for (std::size_t byte = 0U; byte < garbage; ++byte)
                {
                    file.put(static_cast<char>(0x7EU));
                }
            }
            const auto record = blackboxRecord(1000U * (index + 1U));
            file.write(reinterpret_cast<const char *>(record.data()),
                       static_cast<std::streamsize>(record.size()));
        }
    }

    /// @brief Records one CSV pair with the recorder itself.
    /// @param logDir directory to record into
    /// @param rows number of rows to write to each file
    /// @return the recorder, so the caller can read the paths back
    void recordPair(const std::string &logDir, std::size_t rows)
    {
        mark4::StreamRecorder recorder(logDir);
        REQUIRE(recorder.startCsvSession());
        for (std::size_t index = 0U; index < rows; ++index)
        {
            mark4::TelemetryPacket telemetry{};
            telemetry.timestampUs = 1000U * (index + 1U);
            telemetry.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
            telemetry.altitudeM = static_cast<float>(index);
            recorder.onTelemetry(telemetry);

            mark4::SimRawPacket simRaw{};
            simRaw.timestampUs = 1000U * (index + 1U);
            simRaw.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
            simRaw.positionM = {0.0f, 0.0f, static_cast<float>(index)};
            recorder.onSimRaw(simRaw);
        }
        recorder.stopCsvSession();
    }
} // namespace

TEST_CASE("the listing names a recorded pair by the prefix its two files share")
{
    const std::string logDir = scratchDirectory("hub_recordings_pair");
    recordPair(logDir, 3U);
    // A batch log lives in the same directory and is nobody's recording.
    std::ofstream(logDir + "/batch_20260806_134028_i0.log") << "not a recording\n";

    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);
    REQUIRE(recordings.size() == 1U);
    CHECK(recordings[0].kind == "streams");
    CHECK(recordings[0].name.rfind("streams_", 0U) == 0U);
    CHECK(recordings[0].telemetryFile == recordings[0].name + "_telemetry.csv");
    CHECK(recordings[0].simRawFile == recordings[0].name + "_simraw.csv");
    CHECK(recordings[0].sizeBytes > 0U);

    const mark4::HubJson listing = mark4::recordingsToJson(logDir, recordings);
    CHECK(listing["logDir"] == logDir);
    REQUIRE(listing["recordings"].size() == 1U);
    CHECK(listing["recordings"][0]["kind"] == "streams");
    CHECK(listing["recordings"][0].contains("modifiedUnixS"));
}

TEST_CASE("a telemetry csv with no sim raw beside it is still a recording")
{
    const std::string logDir = scratchDirectory("hub_recordings_lonely");
    std::ofstream(logDir + "/streams_20260101_000000_telemetry.csv")
        << mark4::StreamRecorder::TELEMETRY_CSV_HEADER << "\r\n1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0"
        << "\r\n";

    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);
    REQUIRE(recordings.size() == 1U);
    CHECK(recordings[0].name == "streams_20260101_000000");
    CHECK(recordings[0].simRawFile.empty());

    const mark4::HubJson listing = mark4::recordingsToJson(logDir, recordings);
    CHECK(listing["recordings"][0]["simRawFile"].get<std::string>().empty());
}

TEST_CASE("a blackbox file is listed with an estimate of what it holds")
{
    const std::string logDir = scratchDirectory("hub_recordings_blackbox");
    writeBlackbox(logDir + "/board_20260101_000000.m4bb", 4U, 0U);

    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);
    REQUIRE(recordings.size() == 1U);
    CHECK(recordings[0].kind == "blackbox");
    CHECK(recordings[0].name == "board_20260101_000000.m4bb");
    CHECK(recordings[0].sizeBytes == 4U * mark4::BLACKBOX_RECORD_SIZE);
    CHECK(recordings[0].estimatedRecords == 4U);
}

TEST_CASE("an empty or missing log directory lists nothing")
{
    CHECK(mark4::listRecordings(scratchDirectory("hub_recordings_empty")).empty());
    CHECK(mark4::listRecordings("/nowhere/at/all").empty());
}

TEST_CASE("a recorded pair decodes back to the rows the recorder wrote")
{
    const std::string logDir = scratchDirectory("hub_recordings_roundtrip");
    recordPair(logDir, 5U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);
    REQUIRE(recordings.size() == 1U);

    const mark4::HubJson decoded =
        mark4::decodeRecording(logDir, recordings[0], mark4::SampleWindow{});
    CHECK(decoded["kind"] == "streams");
    CHECK(decoded["telemetry"]["total"] == 5U);
    CHECK(decoded["telemetry"]["stride"] == 1U);
    CHECK(decoded["telemetry"]["count"] == 5U);
    // The columns are the header line of the file, spelled exactly as it was
    // written: a page names its axes from them.
    CHECK(decoded["telemetry"]["columns"][0] == "timestamp_us");
    CHECK(decoded["telemetry"]["columns"][15] == "altitude_m");
    CHECK(decoded["telemetry"]["rows"][0][0] == 1000U);
    CHECK(decoded["telemetry"]["rows"][4][15] == 4.0);
    CHECK(decoded["simRaw"]["total"] == 5U);
    CHECK(decoded["simRaw"]["rows"][4][7] == 4.0);
}

TEST_CASE("a decode outside the window answers nothing at all")
{
    const std::string logDir = scratchDirectory("hub_recordings_window");
    recordPair(logDir, 5U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);

    mark4::SampleWindow window;
    window.fromUs = 2000U;
    window.toUs = 4000U;
    const mark4::HubJson decoded = mark4::decodeRecording(logDir, recordings[0], window);
    CHECK(decoded["window"]["fromUs"] == 2000U);
    CHECK(decoded["telemetry"]["total"] == 3U);
    CHECK(decoded["telemetry"]["rows"][0][0] == 2000U);
    CHECK(decoded["telemetry"]["rows"][2][0] == 4000U);
}

TEST_CASE("a decimated decode keeps the first and the last point")
{
    const std::string logDir = scratchDirectory("hub_recordings_stride");
    recordPair(logDir, 100U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);

    mark4::SampleWindow window;
    window.maxPoints = 10U;
    const mark4::HubJson decoded = mark4::decodeRecording(logDir, recordings[0], window);
    const mark4::HubJson &series = decoded["telemetry"];
    CHECK(series["total"] == 100U);
    CHECK(series["stride"] == 10U);
    CHECK(series["count"] == series["rows"].size());
    CHECK(series["rows"].size() <= window.maxPoints + 1U);
    // A curve that starts and ends somewhere else than the data does would
    // be a lie about the data.
    CHECK(series["rows"][0][0] == 1000U);
    CHECK(series["rows"][series["rows"].size() - 1U][0] == 100000U);
}

TEST_CASE("a blackbox decode carries the columns of every recorded field")
{
    const std::string logDir = scratchDirectory("hub_recordings_bbdecode");
    writeBlackbox(logDir + "/board_20260101_000000.m4bb", 3U, 0U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);

    const mark4::HubJson decoded =
        mark4::decodeRecording(logDir, recordings[0], mark4::SampleWindow{});
    CHECK(decoded["kind"] == "blackbox");
    CHECK(decoded["total"] == 3U);
    CHECK(decoded["skippedBytes"] == 0U);
    REQUIRE(decoded["columns"].size() == 15U);
    CHECK(decoded["columns"][0] == "timestamp_us");
    CHECK(decoded["columns"][8] == "kill_switch");
    CHECK(decoded["columns"][14] == "motor_3");
    REQUIRE(decoded["rows"].size() == 3U);
    REQUIRE(decoded["rows"][0].size() == 15U);
    CHECK(decoded["rows"][0][0] == 1000U);
    CHECK(decoded["rows"][0][8] == 1U);
    CHECK(decoded["rows"][2][0] == 3000U);
}

TEST_CASE("a torn blackbox file costs the bytes it tore and nothing more")
{
    const std::string logDir = scratchDirectory("hub_recordings_torn");
    writeBlackbox(logDir + "/board_20260101_000000.m4bb", 3U, 17U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);

    const mark4::HubJson decoded =
        mark4::decodeRecording(logDir, recordings[0], mark4::SampleWindow{});
    CHECK(decoded["total"] == 3U);
    CHECK(decoded["skippedBytes"] == 17U);
    CHECK(decoded["rows"][2][0] == 3000U);
}

TEST_CASE("a recording is addressed by the exact name the listing gave it")
{
    const std::string logDir = scratchDirectory("hub_recordings_address");
    writeBlackbox(logDir + "/board_20260101_000000.m4bb", 1U, 0U);
    const std::vector<mark4::Recording> recordings = mark4::listRecordings(logDir);

    mark4::Recording found;
    CHECK(mark4::findRecording(recordings, "board_20260101_000000.m4bb", found));
    CHECK(!mark4::findRecording(recordings, "board_20260101_000000", found));
    CHECK(!mark4::findRecording(recordings, "../../etc/passwd", found));
    CHECK(!mark4::findRecording(recordings, "/etc/passwd", found));
}
