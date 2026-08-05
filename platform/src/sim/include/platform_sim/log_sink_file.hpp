#pragma once

/// @file
/// @brief File-backed blackbox log sink for the sim variant.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "platform/log_sink.hpp"

namespace mark4
{
    /// Appends blackbox records to a binary file. Owns the stream: it is opened
    /// by init() and closed by the destructor, and the object is neither
    /// copyable nor movable so that the stream has a single owner.
    class LogSinkFile final : public AbsLogSink
    {
      public:
        /// Number of writes between two flushes. Keeps the file close to the
        /// last record when the process is killed before the destructor runs
        /// (debugger stop), without paying one flush per record.
        static constexpr std::uint32_t FLUSH_PERIOD_WRITES = 64U;

        /// @param path target file path; it is not copied, so the string must
        ///        outlive the sink (a literal owned by the composition root)
        explicit LogSinkFile(const char *path)
            : m_path(path)
        {
        }

        ~LogSinkFile() override;

        LogSinkFile(const LogSinkFile &) = delete;
        LogSinkFile &operator=(const LogSinkFile &) = delete;
        LogSinkFile(LogSinkFile &&) = delete;
        LogSinkFile &operator=(LogSinkFile &&) = delete;

        /// @brief Opens the target file for binary writing, truncating any
        ///        previous run. The reason is logged on failure.
        /// @return true when the file is ready to be written to
        bool init();

        /// @brief Appends one record and flushes the stream every
        ///        FLUSH_PERIOD_WRITES writes. Best effort: a write failure is
        ///        logged once and the record is dropped.
        /// @param data record bytes
        /// @param size record size in bytes
        void write(const std::uint8_t *data, std::size_t size) override;

        /// @return target file path
        [[nodiscard]] const char *path() const
        {
            return m_path;
        }

        /// @return total bytes written since construction
        [[nodiscard]] std::size_t bytesWritten() const
        {
            return m_bytesWritten;
        }

      private:
        const char *m_path;                    ///< target file path, not owned
        std::FILE *m_file = nullptr;           ///< nullptr when closed
        std::size_t m_bytesWritten = 0U;       ///< bytes actually written
        std::uint32_t m_writesSinceFlush = 0U; ///< writes since the last flush
        bool m_writeFailureLogged = false;     ///< keeps a full disk from flooding stderr
    };
} // namespace mark4
