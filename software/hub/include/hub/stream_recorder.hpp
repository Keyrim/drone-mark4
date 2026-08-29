#pragma once

/// @file
/// @brief Recording of everything that flows through the hub: the two UDP
///        streams into a CSV pair, the blackbox records of the real board
///        into a .m4bb file. The CSV pair is byte-compatible with what the
///        python recorder writes, down to the header line and the CRLF
///        terminator, so the existing comparison tooling reads either
///        indifferently.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

#include "protocol/sim_raw.hpp"
#include "protocol/telemetry.hpp"

namespace mark4
{
    /// @brief Renders a float the way python renders one in a CSV cell: the
    ///        shortest decimal that reads back as the same double, with a
    ///        trailing ".0" on whole values and the same fixed / exponent
    ///        switch. The wire is single-precision, so the value is widened
    ///        first, exactly as a python consumer unpacking the same bytes.
    /// @param value value to render
    /// @return the decimal text
    std::string formatCsvFloat(float value);

    /// Writes the streams the hub sees to disk. Owns its files and nothing
    /// else: no socket, no thread, so a test drives it end to end.
    class StreamRecorder
    {
      public:
        /// Header line of the telemetry CSV.
        static constexpr const char *TELEMETRY_CSV_HEADER =
            "timestamp_us,gyro_x,gyro_y,gyro_z,quat_w,quat_x,quat_y,quat_z,"
            "bias_x,bias_y,bias_z,motor_0,motor_1,motor_2,motor_3,altitude_m,vz_mps,"
            "baro_altitude_m";

        /// Header line of the sim raw CSV.
        static constexpr const char *SIM_RAW_CSV_HEADER =
            "timestamp_us,quat_w,quat_x,quat_y,quat_z,pos_x,pos_y,pos_z,vel_x,vel_y,vel_z";

        /// Line terminator of both CSV files. Python's csv module writes CRLF
        /// by default and the existing recordings carry it, so a byte-level
        /// comparison of an old and a new file has to agree here too.
        static constexpr const char *CSV_LINE_END = "\r\n";

        /// Size of the buffer holding a timestamped file name.
        static constexpr std::size_t PATH_SIZE = 128U;

        /// What the recorder has written so far.
        struct Stats
        {
            std::uint64_t telemetryRows = 0U;   ///< telemetry CSV rows written
            std::uint64_t simRawRows = 0U;      ///< sim raw CSV rows written
            std::uint64_t blackboxRecords = 0U; ///< blackbox records written
            std::uint64_t badFrames = 0U;       ///< serial frames that decoded to nothing
        };

        /// @param logDirectory directory every file is created in
        explicit StreamRecorder(std::string logDirectory);

        /// @brief Opens a fresh timestamped CSV pair and writes both headers.
        ///        A session already open is closed first.
        /// @return true when both files are open
        bool startCsvSession();

        /// @brief Closes the CSV pair, if one is open.
        void stopCsvSession();

        /// @return true when a CSV session is open
        [[nodiscard]] bool csvSessionOpen() const
        {
            return m_telemetryCsv.is_open() && m_simRawCsv.is_open();
        }

        /// @brief Appends one telemetry row, when a session is open.
        /// @param packet packet to record
        void onTelemetry(const TelemetryPacket &packet);

        /// @brief Appends one sim raw row, when a session is open.
        /// @param packet packet to record
        void onSimRaw(const SimRawPacket &packet);

        /// @brief Appends one blackbox record, opening the file on the first
        ///        one: a session without a board must not leave an empty file
        ///        behind. The bytes are written exactly as they arrived, sync
        ///        marker and CRC included, so the file is a plain sequence of
        ///        self-framing records.
        /// @param record record bytes, already checked by the caller
        /// @param size number of bytes
        /// @return true when the bytes reached the file
        bool onBlackboxRecord(const std::uint8_t *record, std::size_t size);

        /// @brief Counts one serial frame that carried nothing usable.
        void countBadFrame()
        {
            ++m_stats.badFrames;
        }

        /// @return what has been written so far
        [[nodiscard]] const Stats &stats() const
        {
            return m_stats;
        }

        /// @return path of the telemetry CSV of the current session, empty
        ///         when no session was ever started
        [[nodiscard]] const std::string &telemetryCsvPath() const
        {
            return m_telemetryCsvPath;
        }

        /// @return path of the sim raw CSV of the current session
        [[nodiscard]] const std::string &simRawCsvPath() const
        {
            return m_simRawCsvPath;
        }

        /// @return path of the blackbox file, empty until the first record
        [[nodiscard]] const std::string &blackboxPath() const
        {
            return m_blackboxPath;
        }

      private:
        /// @brief Builds a timestamped path inside one subdirectory of the
        ///        log directory.
        /// @param subdir subdirectory the file goes in
        /// @param pattern strftime pattern of the file name
        /// @return the full path
        [[nodiscard]] std::string stampedPath(const char *subdir, const char *pattern) const;

        std::string m_logDirectory;     ///< directory every file is created in
        std::string m_telemetryCsvPath; ///< path of the telemetry CSV
        std::string m_simRawCsvPath;    ///< path of the sim raw CSV
        std::string m_blackboxPath;     ///< path of the blackbox file
        std::ofstream m_telemetryCsv;   ///< telemetry CSV of the current session
        std::ofstream m_simRawCsv;      ///< sim raw CSV of the current session
        std::ofstream m_blackbox;       ///< blackbox file, opened on the first record
        Stats m_stats;                  ///< counters published in the status message
    };
} // namespace mark4
