#include "log/module.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace mark4
{
    namespace
    {
        // Constant-initialized, so a module constructed during the dynamic
        // initialization of another translation unit always finds a valid
        // list head.
        LogModule *g_head = nullptr;
        std::size_t g_count = 0U;
        AbsLogSink *g_sinks[LOG_MAX_SINKS] = {};
        LogClockFn g_clock = nullptr;
        void *g_clockContext = nullptr;

        constexpr const char *LEVEL_NAMES[] = {"TRAC", "DBUG", "INFO", "WARN", "ERR "};

        std::uint64_t now()
        {
            return g_clock != nullptr ? g_clock(g_clockContext) : 0U;
        }
    } // namespace

    const char *logLevelName(LogLevel level)
    {
        const auto index = static_cast<std::size_t>(level);
        return index < std::size(LEVEL_NAMES) ? LEVEL_NAMES[index] : "????";
    }

    LogModule::LogModule(std::uint16_t id, const char *name)
        : m_id(id),
          m_name(name),
          m_next(g_head)
    {
        g_head = this;
        ++g_count;
    }

    void LogModule::vlog(LogLevel level, const char *format, va_list args)
    {
        char text[MAX_TEXT + 1U];
        static_cast<void>(std::vsnprintf(text, sizeof(text), format, args));
        LogRecord record;
        record.moduleId = m_id;
        record.moduleName = m_name;
        record.level = level;
        record.timestampUs = now();
        record.text = text;
        for (AbsLogSink *sink : g_sinks)
        {
            if (sink != nullptr)
            {
                sink->write(record);
            }
        }
    }

// The level check happens here, before the arguments are formatted.
#define MARK4_LOG_LEVEL_FN(method, level)                                                          \
    void LogModule::method(const char *format, ...)                                                \
    {                                                                                              \
        if (!enabled(level))                                                                       \
        {                                                                                          \
            return;                                                                                \
        }                                                                                          \
        va_list args;                                                                              \
        va_start(args, format);                                                                    \
        vlog(level, format, args);                                                                 \
        va_end(args);                                                                              \
    }

    MARK4_LOG_LEVEL_FN(trace, LogLevel::TRACE)
    MARK4_LOG_LEVEL_FN(debug, LogLevel::DEBUG)
    MARK4_LOG_LEVEL_FN(info, LogLevel::INFO)
    MARK4_LOG_LEVEL_FN(warn, LogLevel::WARN)
    MARK4_LOG_LEVEL_FN(error, LogLevel::ERROR)

#undef MARK4_LOG_LEVEL_FN

    void logSetClock(LogClockFn clock, void *context)
    {
        g_clock = clock;
        g_clockContext = context;
    }

    bool logAddSink(AbsLogSink &sink)
    {
        for (AbsLogSink *&slot : g_sinks)
        {
            if (slot == nullptr)
            {
                slot = &sink;
                return true;
            }
        }
        return false;
    }

    void logRemoveSink(AbsLogSink &sink)
    {
        for (AbsLogSink *&slot : g_sinks)
        {
            if (slot == &sink)
            {
                slot = nullptr;
            }
        }
    }

    LogModule *logModules()
    {
        return g_head;
    }

    std::size_t logModuleCount()
    {
        return g_count;
    }

    LogModule *logFindModule(std::uint16_t id)
    {
        for (LogModule *module = g_head; module != nullptr; module = module->next())
        {
            if (module->id() == id)
            {
                return module;
            }
        }
        return nullptr;
    }

    bool logSetLevel(std::uint16_t id, LogLevel level)
    {
        LogModule *module = logFindModule(id);
        if (module == nullptr)
        {
            return false;
        }
        module->setLevel(level);
        return true;
    }

    std::size_t logSetLevelByPrefix(const char *prefix, LogLevel level)
    {
        const std::size_t length = std::strlen(prefix);
        std::size_t touched = 0U;
        for (LogModule *module = g_head; module != nullptr; module = module->next())
        {
            if (std::strncmp(module->name(), prefix, length) == 0)
            {
                module->setLevel(level);
                ++touched;
            }
        }
        return touched;
    }
} // namespace mark4
