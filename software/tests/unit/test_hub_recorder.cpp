/// @file
/// @brief Stream recorder: the CSV pair is what a python consumer would have
///        written itself, so its exact shape is a contract with the wire and
///        not an implementation detail. The expected strings below are
///        spelled out on purpose, and a silent drift has to fail here.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hub/stream_recorder.hpp"
#include "protocol/blackbox.hpp"

namespace
{
    /// Header line of the telemetry CSV, as a python consumer reads it.
    constexpr const char *PYTHON_TELEMETRY_HEADER =
        "timestamp_us,gyro_x,gyro_y,gyro_z,quat_w,quat_x,quat_y,quat_z,"
        "bias_x,bias_y,bias_z,motor_0,motor_1,motor_2,motor_3,altitude_m,vz_mps,"
        "baro_altitude_m";

    /// Header line of the sim raw CSV, as a python consumer reads it.
    constexpr const char *PYTHON_SIM_RAW_HEADER =
        "timestamp_us,quat_w,quat_x,quat_y,quat_z,pos_x,pos_y,pos_z,vel_x,vel_y,vel_z";

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

    /// @brief Reads a whole file.
    /// @param path file to read
    /// @return its bytes
    std::string readFile(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    /// @brief Splits text on the CRLF terminator python's csv module writes.
    /// @param text text to split
    /// @return the lines, without their terminator
    std::vector<std::string> crlfLines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::size_t start = 0U;
        while (start < text.size())
        {
            const std::size_t end = text.find("\r\n", start);
            if (end == std::string::npos)
            {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, end - start));
            start = end + 2U;
        }
        return lines;
    }

    /// @brief Builds one valid blackbox record.
    /// @param timestampUs acquisition time to carry [us]
    /// @return the serialized record bytes
    std::array<std::uint8_t, mark4::BLACKBOX_RECORD_SIZE> blackboxRecord(std::uint64_t timestampUs)
    {
        mark4::BlackboxRecord record;
        record.sync0 = mark4::BLACKBOX_SYNC0;
        record.sync1 = mark4::BLACKBOX_SYNC1;
        record.version = mark4::BLACKBOX_VERSION;
        record.type = static_cast<std::uint8_t>(mark4::PacketType::BLACKBOX_RECORD);
        record.length = mark4::BLACKBOX_RECORD_PAYLOAD_SIZE;
        record.timestampUs = timestampUs;
        record.gyroRadS = {0.25f, -0.5f, 0.75f};
        record.accelMps2 = {0.0f, 0.0f, 9.81f};
        record.baroPa = 101325.0f;
        record.throttle = 0.5f;
        record.motor = {0.1f, 0.2f, 0.3f, 0.4f};

        std::array<std::uint8_t, mark4::BLACKBOX_RECORD_SIZE> bytes{};
        std::memcpy(bytes.data(), &record, bytes.size());
        const std::uint16_t crc = mark4::blackboxRecordCrc(bytes.data());
        bytes[mark4::BLACKBOX_CRC_OFFSET] = static_cast<std::uint8_t>(crc & mark4::CRC16_LOW_MASK);
        bytes[mark4::BLACKBOX_CRC_OFFSET + 1U] =
            static_cast<std::uint8_t>(crc >> mark4::CRC16_BYTE_SHIFT);
        return bytes;
    }
} // namespace

TEST_CASE("a csv cell reads exactly like the one python writes")
{
    // Left column: the float32 value. Right column: repr() of the double a
    // python consumer gets by unpacking the same four bytes.
    CHECK(mark4::formatCsvFloat(0.0f) == "0.0");
    CHECK(mark4::formatCsvFloat(-0.0f) == "-0.0");
    CHECK(mark4::formatCsvFloat(1.0f) == "1.0");
    CHECK(mark4::formatCsvFloat(123.0f) == "123.0");
    CHECK(mark4::formatCsvFloat(0.5f) == "0.5");
    CHECK(mark4::formatCsvFloat(-3.25f) == "-3.25");
    CHECK(mark4::formatCsvFloat(65504.0f) == "65504.0");
    CHECK(mark4::formatCsvFloat(0.1f) == "0.10000000149011612");
    CHECK(mark4::formatCsvFloat(12345.678f) == "12345.677734375");
    CHECK(mark4::formatCsvFloat(1e-5f) == "9.999999747378752e-06");
    CHECK(mark4::formatCsvFloat(1e-4f) == "9.999999747378752e-05");
    CHECK(mark4::formatCsvFloat(1.5e-7f) == "1.500000053056283e-07");
    CHECK(mark4::formatCsvFloat(2.5e-8f) == "2.5000000292152436e-08");
    CHECK(mark4::formatCsvFloat(1e15f) == "999999986991104.0");
    CHECK(mark4::formatCsvFloat(1e16f) == "1.0000000272564224e+16");
    CHECK(mark4::formatCsvFloat(1e20f) == "1.0000000200408773e+20");
    CHECK(mark4::formatCsvFloat(1e-30f) == "1.0000000031710769e-30");
    CHECK(mark4::formatCsvFloat(3.4028235e38f) == "3.4028234663852886e+38");
}

TEST_CASE("the csv headers are the ones the python reader expects")
{
    CHECK(std::string(mark4::StreamRecorder::TELEMETRY_CSV_HEADER) == PYTHON_TELEMETRY_HEADER);
    CHECK(std::string(mark4::StreamRecorder::SIM_RAW_CSV_HEADER) == PYTHON_SIM_RAW_HEADER);
    CHECK(std::string(mark4::StreamRecorder::CSV_LINE_END) == "\r\n");

    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_headers"));
    REQUIRE(recorder.startCsvSession());
    recorder.stopCsvSession();

    CHECK(readFile(recorder.telemetryCsvPath()) == std::string(PYTHON_TELEMETRY_HEADER) + "\r\n");
    CHECK(readFile(recorder.simRawCsvPath()) == std::string(PYTHON_SIM_RAW_HEADER) + "\r\n");
}

TEST_CASE("a telemetry row holds the columns the python reader expects")
{
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_telemetry"));
    REQUIRE(recorder.startCsvSession());

    mark4::TelemetryPacket packet{};
    packet.timestampUs = 1'234'567U;
    packet.gyroRadS = {0.25f, -0.5f, 0.75f};
    packet.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
    packet.gyroBiasRadS = {0.1f, 0.0f, -0.1f};
    packet.motor = {0.0f, 0.25f, 0.5f, 1.0f};
    packet.altitudeM = 12.5f;
    packet.verticalVelocityMps = -3.25f;
    packet.baroAltitudeM = 11.75f;
    recorder.onTelemetry(packet);
    recorder.onTelemetry(packet);
    recorder.stopCsvSession();

    const std::vector<std::string> lines = crlfLines(readFile(recorder.telemetryCsvPath()));
    REQUIRE(lines.size() == 3U);
    CHECK(lines[0] == PYTHON_TELEMETRY_HEADER);
    CHECK(lines[1] == "1234567,0.25,-0.5,0.75,1.0,0.0,0.0,0.0,"
                      "0.10000000149011612,0.0,-0.10000000149011612,"
                      "0.0,0.25,0.5,1.0,12.5,-3.25,11.75");
    CHECK(lines[2] == lines[1]);
    CHECK(recorder.stats().telemetryRows == 2U);
}

TEST_CASE("a sim raw row holds the columns the python reader expects")
{
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_simraw"));
    REQUIRE(recorder.startCsvSession());

    mark4::SimRawPacket packet{};
    packet.timestampUs = 42U;
    packet.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
    packet.positionM = {-1.5f, 2.5f, 3.5f};
    packet.velocityMps = {0.0f, 0.0f, -9.75f};
    recorder.onSimRaw(packet);
    recorder.stopCsvSession();

    const std::vector<std::string> lines = crlfLines(readFile(recorder.simRawCsvPath()));
    REQUIRE(lines.size() == 2U);
    CHECK(lines[0] == PYTHON_SIM_RAW_HEADER);
    CHECK(lines[1] == "42,1.0,0.0,0.0,0.0,-1.5,2.5,3.5,0.0,0.0,-9.75");
    CHECK(recorder.stats().simRawRows == 1U);
}

TEST_CASE("a packet whose fields are misaligned records like any other")
{
    // The wire structs are packed to the byte, so a float array of theirs
    // sits wherever the layout puts it. Recording a packet shifted by every
    // byte offset in turn fails the sanitizer build the moment something
    // binds a reference to one of those fields.
    static constexpr std::size_t OFFSET_COUNT = 8U;
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_misaligned"));
    REQUIRE(recorder.startCsvSession());

    mark4::TelemetryPacket packet{};
    packet.timestampUs = 7U;
    packet.gyroRadS = {0.25f, -0.5f, 0.75f};
    packet.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
    packet.gyroBiasRadS = {0.1f, 0.0f, -0.1f};
    packet.motor = {0.0f, 0.25f, 0.5f, 1.0f};
    packet.altitudeM = 12.5f;
    packet.verticalVelocityMps = -3.25f;

    recorder.onTelemetry(packet);
    std::vector<std::uint8_t> storage(sizeof(packet) + OFFSET_COUNT, 0U);
    for (std::size_t offset = 0U; offset < OFFSET_COUNT; ++offset)
    {
        std::memcpy(&storage[offset], &packet, sizeof(packet));
        recorder.onTelemetry(*reinterpret_cast<const mark4::TelemetryPacket *>(&storage[offset]));
    }
    recorder.stopCsvSession();

    const std::vector<std::string> lines = crlfLines(readFile(recorder.telemetryCsvPath()));
    REQUIRE(lines.size() == OFFSET_COUNT + 2U);
    for (std::size_t row = 2U; row < lines.size(); ++row)
    {
        CHECK(lines[row] == lines[1]);
    }
}

TEST_CASE("nothing is written while no csv session is open")
{
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_idle"));
    mark4::TelemetryPacket packet{};
    recorder.onTelemetry(packet);
    CHECK(recorder.stats().telemetryRows == 0U);
    CHECK(recorder.telemetryCsvPath().empty());
}

TEST_CASE("blackbox records are stored verbatim and stay decodable")
{
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_blackbox"));
    CHECK(recorder.blackboxPath().empty());

    const auto first = blackboxRecord(1000U);
    const auto second = blackboxRecord(2000U);
    REQUIRE(mark4::validBlackboxRecord(first.data()));
    REQUIRE(recorder.onBlackboxRecord(first.data(), first.size()));
    REQUIRE(recorder.onBlackboxRecord(second.data(), second.size()));
    CHECK(recorder.stats().blackboxRecords == 2U);
    REQUIRE(!(recorder.blackboxPath().empty()));

    const std::string stored = readFile(recorder.blackboxPath());
    REQUIRE(stored.size() == 2U * mark4::BLACKBOX_RECORD_SIZE);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(stored.data());
    CHECK(std::memcmp(bytes, first.data(), first.size()) == 0);
    CHECK(std::memcmp(&bytes[mark4::BLACKBOX_RECORD_SIZE], second.data(), second.size()) == 0);
    CHECK(mark4::validBlackboxRecord(bytes));
    CHECK(mark4::validBlackboxRecord(&bytes[mark4::BLACKBOX_RECORD_SIZE]));
}

TEST_CASE("a new csv session starts a new pair of files")
{
    mark4::StreamRecorder recorder(scratchDirectory("hub_recorder_sessions"));
    REQUIRE(recorder.startCsvSession());
    CHECK(recorder.csvSessionOpen());
    recorder.stopCsvSession();
    CHECK(!(recorder.csvSessionOpen()));
    REQUIRE(recorder.startCsvSession());
    CHECK(recorder.csvSessionOpen());
}
