#include "platform_sim/log_sink_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mark4
{
    namespace
    {
        /// @brief Reports a failed file operation and the reason behind it.
        /// @param what name of the operation that failed
        /// @param path file the operation was applied to
        void logErrno(const char *what, const char *path)
        {
            static_cast<void>(std::fprintf(
                stderr, "LogSinkFile: %s failed on '%s': %s\n", what, path, std::strerror(errno)));
        }
    } // namespace

    LogSinkFile::~LogSinkFile()
    {
        if (m_file != nullptr)
        {
            static_cast<void>(std::fclose(m_file));
            m_file = nullptr;
        }
    }

    bool LogSinkFile::init()
    {
        if (m_file != nullptr)
        {
            static_cast<void>(std::fprintf(stderr, "LogSinkFile: already open\n"));
            return false;
        }
        if (m_path == nullptr)
        {
            static_cast<void>(std::fprintf(stderr, "LogSinkFile: no path given\n"));
            return false;
        }

        m_file = std::fopen(m_path, "wb");
        if (m_file == nullptr)
        {
            logErrno("fopen", m_path);
            return false;
        }
        return true;
    }

    void LogSinkFile::write(const std::uint8_t *data, std::size_t size)
    {
        if (m_file == nullptr || data == nullptr || size == 0U)
        {
            return;
        }

        const std::size_t written = std::fwrite(data, 1U, size, m_file);
        if (written != size)
        {
            if (!m_writeFailureLogged)
            {
                logErrno("fwrite", m_path);
                m_writeFailureLogged = true;
            }
        }
        m_bytesWritten += written;

        ++m_writesSinceFlush;
        if (m_writesSinceFlush >= FLUSH_PERIOD_WRITES)
        {
            static_cast<void>(std::fflush(m_file));
            m_writesSinceFlush = 0U;
        }
    }
} // namespace mark4
