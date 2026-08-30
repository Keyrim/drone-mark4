#include "log/console_sink_posix.hpp"

#include <cstdint>
#include <cstdio>

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;
        constexpr std::uint64_t MS_PER_S = 1000U;
        constexpr std::uint64_t S_PER_MIN = 60U;
        constexpr std::uint64_t MIN_PER_HOUR = 60U;
    } // namespace

    void ConsoleSinkPosix::write(const LogRecord &record)
    {
        const std::uint64_t ms = record.timestampUs / US_PER_MS;
        const std::uint64_t s = ms / MS_PER_S;
        const std::uint64_t min = s / S_PER_MIN;
        static_cast<void>(std::printf("%02llu:%02llu:%02llu.%03llu %s %s: %s\n",
                                      static_cast<unsigned long long>(min / MIN_PER_HOUR),
                                      static_cast<unsigned long long>(min % MIN_PER_HOUR),
                                      static_cast<unsigned long long>(s % S_PER_MIN),
                                      static_cast<unsigned long long>(ms % MS_PER_S),
                                      logLevelName(record.level),
                                      record.moduleName,
                                      record.text));
        static_cast<void>(std::fflush(stdout));
    }
} // namespace mark4
