/// @file
/// @brief Recording listing and decoding implementation.

#include "hub/recordings.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "hub/packed_field.hpp"
#include "protocol/blackbox.hpp"

namespace mark4
{
    namespace
    {
        /// Suffix of the telemetry half of a recorded pair.
        constexpr const char *TELEMETRY_SUFFIX = "_telemetry.csv";

        /// Suffix of the exact-state half of a recorded pair.
        constexpr const char *SIM_RAW_SUFFIX = "_simraw.csv";

        /// Extension of a blackbox file.
        constexpr const char *BLACKBOX_EXTENSION = ".m4bb";

        /// Bytes read from a blackbox file at a time.
        constexpr std::size_t READ_CHUNK = 65536U;

        /// Base a recorded number is written in.
        constexpr int DECIMAL = 10;

        /// Columns of the CSV rendering of a blackbox record.
        constexpr const char *BLACKBOX_CSV_HEADER =
            "timestamp_us,gyro_x_rad_s,gyro_y_rad_s,gyro_z_rad_s,"
            "accel_x_mps2,accel_y_mps2,accel_z_mps2,baro_pa,kill_switch,throttle,arm_switch,"
            "motor_0,motor_1,motor_2,motor_3";

        /// Walks the records of a .m4bb file without ever holding the file.
        /// A torn write costs the record it tore and nothing more: the walk
        /// steps forward one byte at a time until the framing checks out
        /// again, and counts what it stepped over.
        class BlackboxReader
        {
          public:
            /// @param path file to walk
            explicit BlackboxReader(const std::string &path)
                : m_file(path, std::ios::binary)
            {
            }

            /// @return true when the file could be opened
            [[nodiscard]] bool isOpen() const
            {
                return m_file.is_open();
            }

            /// @brief Reads the next valid record.
            /// @param recordOut receives the record
            /// @return true while records remain
            bool next(BlackboxRecord &recordOut)
            {
                while (true)
                {
                    if (m_buffer.size() - m_cursor < BLACKBOX_RECORD_SIZE && !refill())
                    {
                        // Whatever is left cannot frame a record: it is the
                        // tail of a write that was cut short.
                        m_skippedBytes += m_buffer.size() - m_cursor;
                        m_cursor = m_buffer.size();
                        return false;
                    }
                    const std::uint8_t *at = &m_buffer[m_cursor];
                    if (validBlackboxRecord(at))
                    {
                        std::memcpy(&recordOut, at, BLACKBOX_RECORD_SIZE);
                        m_cursor += BLACKBOX_RECORD_SIZE;
                        return true;
                    }
                    ++m_cursor;
                    ++m_skippedBytes;
                }
            }

            /// @return bytes that framed no record
            [[nodiscard]] std::uint64_t skippedBytes() const
            {
                return m_skippedBytes;
            }

          private:
            /// @brief Drops what has been consumed and reads more.
            /// @return true when a whole record is available
            bool refill()
            {
                m_buffer.erase(m_buffer.begin(),
                               m_buffer.begin() + static_cast<std::ptrdiff_t>(m_cursor));
                m_cursor = 0U;
                while (m_buffer.size() < BLACKBOX_RECORD_SIZE && m_file.good())
                {
                    const std::size_t had = m_buffer.size();
                    m_buffer.resize(had + READ_CHUNK);
                    m_file.read(reinterpret_cast<char *>(&m_buffer[had]),
                                static_cast<std::streamsize>(READ_CHUNK));
                    m_buffer.resize(had + static_cast<std::size_t>(m_file.gcount()));
                    if (m_file.gcount() == 0)
                    {
                        break;
                    }
                }
                return m_buffer.size() >= BLACKBOX_RECORD_SIZE;
            }

            std::ifstream m_file;               ///< the file being walked
            std::vector<std::uint8_t> m_buffer; ///< bytes read but not consumed
            std::size_t m_cursor = 0U;          ///< where in the buffer the walk sits
            std::uint64_t m_skippedBytes = 0U;  ///< bytes that framed no record
        };

        /// @brief Step between the points kept, so the answer holds no more
        ///        than what was asked for.
        /// @param total points in the window
        /// @param maxPoints points wanted at most
        /// @return the step, never zero
        std::size_t strideFor(std::size_t total, std::size_t maxPoints)
        {
            const std::size_t wanted = std::max<std::size_t>(1U, maxPoints);
            if (total <= wanted)
            {
                return 1U;
            }
            return (total + wanted - 1U) / wanted;
        }

        /// @brief Whether one point of the window is kept. The first and the
        ///        last always are: a curve that starts and ends somewhere
        ///        else than the data does is a lie about the data.
        /// @param index index of the point in the window
        /// @param total points in the window
        /// @param stride step between kept points
        /// @return true when the point is kept
        bool keepPoint(std::size_t index, std::size_t total, std::size_t stride)
        {
            return index % stride == 0U || index + 1U == total;
        }

        /// @brief Renders one CSV cell as a JSON number, keeping a whole
        ///        number whole.
        /// @param cell text to render
        /// @return the number
        HubJson cellToJson(const std::string &cell)
        {
            // Braces would build a one-element array here, not a number: the
            // JSON library reads an initializer list as one.
            if (cell.find_first_of(".eEnN") == std::string::npos && !cell.empty())
            {
                HubJson whole = std::strtoll(cell.c_str(), nullptr, DECIMAL);
                return whole;
            }
            HubJson fractional = std::strtod(cell.c_str(), nullptr);
            return fractional;
        }

        /// @brief Splits one CSV line on its commas.
        /// @param line line to split
        /// @return the cells
        std::vector<std::string> csvCells(const std::string &line)
        {
            std::vector<std::string> cells;
            std::size_t start = 0U;
            while (start <= line.size())
            {
                const std::size_t comma = line.find(',', start);
                if (comma == std::string::npos)
                {
                    cells.push_back(line.substr(start));
                    break;
                }
                cells.push_back(line.substr(start, comma - start));
                start = comma + 1U;
            }
            return cells;
        }

        /// @brief Reads one line, without the terminator the recorder wrote.
        /// @param file file to read from
        /// @param lineOut receives the line
        /// @return true while lines remain
        bool readCsvLine(std::istream &file, std::string &lineOut)
        {
            if (!std::getline(file, lineOut))
            {
                return false;
            }
            if (!lineOut.empty() && lineOut.back() == '\r')
            {
                lineOut.pop_back();
            }
            return true;
        }

        /// @brief Decodes one stream CSV, twice: once to count what falls in
        ///        the window, once to emit every n-th row of it.
        /// @param path file to decode
        /// @param window part of it to cover
        /// @return the JSON series, null when the file cannot be read
        HubJson decodeStreamCsv(const std::string &path, const SampleWindow &window)
        {
            std::ifstream counting(path, std::ios::binary);
            if (!counting.is_open())
            {
                return {};
            }
            std::string line;
            std::string header;
            static_cast<void>(readCsvLine(counting, header));
            std::size_t total = 0U;
            while (readCsvLine(counting, line))
            {
                if (line.empty())
                {
                    continue;
                }
                const auto timestamp = std::strtoull(line.c_str(), nullptr, 10);
                if (timestamp >= window.fromUs && timestamp <= window.toUs)
                {
                    ++total;
                }
            }

            HubJson columns = HubJson::array();
            for (const std::string &name : csvCells(header))
            {
                columns.push_back(name);
            }

            const std::size_t stride = strideFor(total, window.maxPoints);
            HubJson rows = HubJson::array();
            std::ifstream emitting(path, std::ios::binary);
            static_cast<void>(readCsvLine(emitting, header));
            std::size_t index = 0U;
            while (readCsvLine(emitting, line))
            {
                if (line.empty())
                {
                    continue;
                }
                const auto timestamp = std::strtoull(line.c_str(), nullptr, 10);
                if (timestamp < window.fromUs || timestamp > window.toUs)
                {
                    continue;
                }
                if (keepPoint(index, total, stride))
                {
                    HubJson row = HubJson::array();
                    for (const std::string &cell : csvCells(line))
                    {
                        row.push_back(cellToJson(cell));
                    }
                    rows.push_back(row);
                }
                ++index;
            }

            HubJson series;
            series["total"] = total;
            series["stride"] = stride;
            series["count"] = rows.size();
            series["columns"] = columns;
            series["rows"] = rows;
            return series;
        }

        /// @brief Appends the fifteen values of one record to a JSON row.
        /// @param record record to render
        /// @param rowOut receives the values
        void blackboxRowToJson(const BlackboxRecord &record, HubJson &rowOut)
        {
            // Every field is copied out of the packed record before anything
            // takes its address: a field of a packed struct sits wherever the
            // layout puts it, and a reference to it is not addressable.
            const auto gyro = readPackedField(&record.gyroRadS);
            const auto accel = readPackedField(&record.accelMps2);
            const auto motor = readPackedField(&record.motor);
            const std::uint64_t timestampUs = record.timestampUs;
            const std::uint8_t killSwitch = record.killSwitch;
            const std::uint8_t armSwitch = record.armSwitch;
            rowOut.push_back(timestampUs);
            for (const float value : gyro)
            {
                rowOut.push_back(static_cast<double>(value));
            }
            for (const float value : accel)
            {
                rowOut.push_back(static_cast<double>(value));
            }
            rowOut.push_back(static_cast<double>(record.baroPa));
            rowOut.push_back(killSwitch);
            rowOut.push_back(static_cast<double>(record.throttle));
            rowOut.push_back(armSwitch);
            for (const float value : motor)
            {
                rowOut.push_back(static_cast<double>(value));
            }
        }

        /// @brief Decodes one blackbox file, twice, the same way a stream CSV
        ///        is decoded and for the same reason.
        /// @param path file to decode
        /// @param window part of it to cover
        /// @param answerOut receives total, stride, count, skippedBytes,
        ///        columns and rows
        /// @return true when the file could be read
        bool decodeBlackbox(const std::string &path, const SampleWindow &window, HubJson &answerOut)
        {
            BlackboxReader counting(path);
            if (!counting.isOpen())
            {
                return false;
            }
            BlackboxRecord record{};
            std::size_t total = 0U;
            while (counting.next(record))
            {
                if (record.timestampUs >= window.fromUs && record.timestampUs <= window.toUs)
                {
                    ++total;
                }
            }

            const std::size_t stride = strideFor(total, window.maxPoints);
            HubJson rows = HubJson::array();
            BlackboxReader emitting(path);
            std::size_t index = 0U;
            while (emitting.next(record))
            {
                if (record.timestampUs < window.fromUs || record.timestampUs > window.toUs)
                {
                    continue;
                }
                if (keepPoint(index, total, stride))
                {
                    HubJson row = HubJson::array();
                    blackboxRowToJson(record, row);
                    rows.push_back(row);
                }
                ++index;
            }

            HubJson columns = HubJson::array();
            for (const std::string &name : csvCells(BLACKBOX_CSV_HEADER))
            {
                columns.push_back(name);
            }
            answerOut["total"] = total;
            answerOut["stride"] = stride;
            answerOut["count"] = rows.size();
            answerOut["skippedBytes"] = counting.skippedBytes();
            answerOut["columns"] = columns;
            answerOut["rows"] = rows;
            return true;
        }

        /// @brief Last write of a file, in seconds since the epoch.
        /// @param path file to stamp
        /// @return the stamp, 0 when it cannot be read
        std::int64_t modifiedUnixS(const std::filesystem::path &path)
        {
            std::error_code failure;
            const auto written = std::filesystem::last_write_time(path, failure);
            if (failure)
            {
                return 0;
            }
            const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
            return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch())
                .count();
        }

        /// @brief Size of a file.
        /// @param path file to size
        /// @return its size in bytes, 0 when it cannot be read
        std::uint64_t sizeOf(const std::filesystem::path &path)
        {
            std::error_code failure;
            const auto size = std::filesystem::file_size(path, failure);
            return failure ? 0U : static_cast<std::uint64_t>(size);
        }

    } // namespace

    std::vector<Recording> listRecordings(const std::string &logDir)
    {
        std::vector<Recording> recordings;
        std::error_code failure;
        std::filesystem::directory_iterator entries(logDir, failure);
        if (failure)
        {
            return recordings;
        }

        for (const std::filesystem::directory_entry &entry : entries)
        {
            if (!entry.is_regular_file(failure))
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() == BLACKBOX_EXTENSION)
            {
                Recording recording;
                recording.name = name;
                recording.kind = "blackbox";
                recording.sizeBytes = sizeOf(entry.path());
                recording.modifiedUnixS = modifiedUnixS(entry.path());
                // An estimate, not a count: counting means decoding the whole
                // file, and a listing must stay cheap however long the run.
                recording.estimatedRecords = recording.sizeBytes / BLACKBOX_RECORD_SIZE;
                recordings.push_back(recording);
                continue;
            }
            const std::size_t suffix = name.rfind(TELEMETRY_SUFFIX);
            if (suffix == std::string::npos ||
                suffix + std::strlen(TELEMETRY_SUFFIX) != name.size())
            {
                // Everything else in the directory belongs to somebody else:
                // the sim raw half is found from its telemetry half, and a
                // batch log is not a recording.
                continue;
            }
            Recording recording;
            recording.name = name.substr(0U, suffix);
            recording.kind = "streams";
            recording.telemetryFile = name;
            recording.sizeBytes = sizeOf(entry.path());
            recording.modifiedUnixS = modifiedUnixS(entry.path());

            const std::filesystem::path simRaw =
                entry.path().parent_path() / (recording.name + SIM_RAW_SUFFIX);
            if (std::filesystem::is_regular_file(simRaw, failure))
            {
                recording.simRawFile = simRaw.filename().string();
                recording.sizeBytes += sizeOf(simRaw);
                recording.modifiedUnixS = std::max(recording.modifiedUnixS, modifiedUnixS(simRaw));
            }
            recordings.push_back(recording);
        }

        std::sort(recordings.begin(),
                  recordings.end(),
                  [](const Recording &left, const Recording &right) {
                      if (left.modifiedUnixS != right.modifiedUnixS)
                      {
                          return left.modifiedUnixS > right.modifiedUnixS;
                      }
                      return left.name > right.name;
                  });
        return recordings;
    }

    bool findRecording(const std::vector<Recording> &recordings,
                       const std::string &name,
                       Recording &recordingOut)
    {
        const auto found =
            std::find_if(recordings.begin(), recordings.end(), [&name](const Recording &recording) {
                return recording.name == name;
            });
        if (found == recordings.end())
        {
            return false;
        }
        recordingOut = *found;
        return true;
    }

    HubJson recordingsToJson(const std::string &logDir, const std::vector<Recording> &recordings)
    {
        HubJson entries = HubJson::array();
        for (const Recording &recording : recordings)
        {
            HubJson entry;
            entry["name"] = recording.name;
            entry["kind"] = recording.kind;
            entry["sizeBytes"] = recording.sizeBytes;
            entry["modifiedUnixS"] = recording.modifiedUnixS;
            if (recording.kind == "blackbox")
            {
                entry["estimatedRecords"] = recording.estimatedRecords;
            }
            else
            {
                entry["telemetryFile"] = recording.telemetryFile;
                entry["simRawFile"] = recording.simRawFile;
            }
            entries.push_back(entry);
        }

        HubJson answer;
        answer["logDir"] = logDir;
        answer["recordings"] = entries;
        return answer;
    }

    HubJson decodeRecording(const std::string &logDir,
                            const Recording &recording,
                            const SampleWindow &window)
    {
        const std::filesystem::path directory(logDir);
        HubJson answer;
        answer["name"] = recording.name;
        answer["kind"] = recording.kind;
        if (recording.kind == "blackbox")
        {
            if (!decodeBlackbox((directory / recording.name).string(), window, answer))
            {
                return {};
            }
            return answer;
        }

        HubJson windowJson;
        windowJson["fromUs"] = window.fromUs;
        windowJson["toUs"] = window.toUs;
        answer["window"] = windowJson;
        const HubJson telemetry =
            decodeStreamCsv((directory / recording.telemetryFile).string(), window);
        if (telemetry.is_null())
        {
            return {};
        }
        answer["telemetry"] = telemetry;
        answer["simRaw"] =
            recording.simRawFile.empty()
                ? HubJson()
                : decodeStreamCsv((directory / recording.simRawFile).string(), window);
        return answer;
    }

    bool readWholeFile(const std::string &path, std::string &contentOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }
        contentOut.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }
} // namespace mark4
