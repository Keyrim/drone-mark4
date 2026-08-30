#pragma once

/// @file
/// @brief One logging module per source file, filterable by level at
///        runtime, and the registry every node exposes over the wire.
///
/// Usage, once per source file:
/// @code
///     namespace { mark4::LogModule MODULE{LOG_MODULE_PLATFORM_IMU, "platform/imu"}; }
///     MODULE.info("found at 0x%02X", address);
/// @endcode
/// The level check happens before any formatting; the text is formatted
/// with snprintf into a fixed buffer and truncated, never allocated. Not
/// thread-safe: every node of the project logs from its one loop thread.

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "log/sink.hpp"

namespace mark4
{
    class LogModule
    {
      public:
        /// Longest module name, terminator excluded (mark4.LogModuleInfo.name).
        static constexpr std::size_t MAX_NAME = 32U;

        /// Longest line, terminator excluded (mark4.Log.text).
        static constexpr std::size_t MAX_TEXT = 96U;

        /// Level a module starts at.
        static constexpr LogLevel DEFAULT_LEVEL = LogLevel::INFO;

        /// @brief Registers the module: links it into the process-wide list
        ///        the runtime enumerates. Meant for a static object.
        /// @param id module id, unique on this node (log/module_ids.hpp)
        /// @param name hierarchical name, "area/thing", at most MAX_NAME
        ///        characters, kept by pointer (a literal)
        LogModule(std::uint16_t id, const char *name);

        LogModule(const LogModule &) = delete;
        LogModule &operator=(const LogModule &) = delete;
        LogModule(LogModule &&) = delete;
        LogModule &operator=(LogModule &&) = delete;
        ~LogModule() = default;

        [[nodiscard]] std::uint16_t id() const
        {
            return m_id;
        }

        [[nodiscard]] const char *name() const
        {
            return m_name;
        }

        [[nodiscard]] LogLevel level() const
        {
            return m_level;
        }

        void setLevel(LogLevel level)
        {
            m_level = level;
        }

        /// @return true when a line of that level would be written
        [[nodiscard]] bool enabled(LogLevel level) const
        {
            return level >= m_level;
        }

        /// @return next module of the registry, nullptr at the end
        [[nodiscard]] LogModule *next() const
        {
            return m_next;
        }

        void trace(const char *format, ...) __attribute__((format(printf, 2, 3)));
        void debug(const char *format, ...) __attribute__((format(printf, 2, 3)));
        void info(const char *format, ...) __attribute__((format(printf, 2, 3)));
        void warn(const char *format, ...) __attribute__((format(printf, 2, 3)));
        void error(const char *format, ...) __attribute__((format(printf, 2, 3)));

        /// @brief Formats and dispatches one line, whatever the level: the
        ///        five methods above are the gate.
        /// @param level severity
        /// @param format printf format
        /// @param args its arguments
        void vlog(LogLevel level, const char *format, va_list args);

      private:
        std::uint16_t m_id;               ///< wire id
        const char *m_name;               ///< hierarchical name
        LogLevel m_level = DEFAULT_LEVEL; ///< threshold, lines below are dropped
        LogModule *m_next = nullptr;      ///< registry link
    };

    /// Clock the records are stamped with: registered once at init, the
    /// library never reads a clock itself.
    using LogClockFn = std::uint64_t (*)(void *context);

    /// Sinks registered at once, at most.
    inline constexpr std::size_t LOG_MAX_SINKS = 2U;

    /// @brief Registers the clock. Until then records carry 0.
    void logSetClock(LogClockFn clock, void *context);

    /// @brief Adds a sink. The sink must outlive its registration.
    /// @return false when LOG_MAX_SINKS are already registered
    bool logAddSink(AbsLogSink &sink);

    /// @brief Removes a sink; unknown sinks are ignored.
    void logRemoveSink(AbsLogSink &sink);

    /// @return first module of the registry, nullptr when none is declared
    LogModule *logModules();

    /// @return registered module count
    std::size_t logModuleCount();

    /// @return the module with that id, nullptr when none
    LogModule *logFindModule(std::uint16_t id);

    /// @brief Sets the level of one module.
    /// @return false when no module has that id
    bool logSetLevel(std::uint16_t id, LogLevel level);

    /// @brief Sets the level of every module whose name starts with prefix
    ///        ("" = all, "platform" = platform/imu, platform/baro, ...).
    /// @return modules touched
    std::size_t logSetLevelByPrefix(const char *prefix, LogLevel level);
} // namespace mark4
