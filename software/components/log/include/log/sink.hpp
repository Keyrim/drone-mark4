#pragma once

/// @file
/// @brief What a log line is once formatted, and where it goes.

#include <cstdint>

namespace mark4
{
    /// Severity of a line, lowest first. The wire (mark4.LogLevel) uses the
    /// same values.
    enum class LogLevel : std::uint8_t
    {
        TRACE = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
    };

    /// @return the four-letter name of a level ("INFO", "WARN", ...)
    const char *logLevelName(LogLevel level);

    /// One formatted line, handed to every sink. The pointers are valid for
    /// the duration of the write only.
    struct LogRecord
    {
        std::uint16_t moduleId = 0U;     ///< id of the module that spoke
        const char *moduleName = "";     ///< its hierarchical name
        LogLevel level = LogLevel::INFO; ///< severity
        std::uint64_t timestampUs = 0U;  ///< instant, from the registered clock [us]
        const char *text = "";           ///< the line, at most LogModule::MAX_TEXT chars
    };

    /// Where lines end up: a console, a debug probe, the transport.
    class AbsLogSink
    {
      public:
        virtual ~AbsLogSink() = default;

        /// @brief Writes one line. Called for every record whose level
        ///        passed its module's threshold; a sink filters no further.
        /// @param record the line
        virtual void write(const LogRecord &record) = 0;
    };
} // namespace mark4
