#pragma once

/// @file
/// @brief Log sink over the RTT console: "t_ms LEVL module: text", one
///        line per record, integer conversions only (newlib-nano).

#include <cstdint>
#include <cstdio>

#include "log/module.hpp"
#include "platform_stm32/rtt.hpp"

namespace mark4
{
    class RttSink final : public AbsLogSink
    {
      public:
        /// Stack buffer of one line: the text plus its prefix.
        static constexpr std::size_t LINE_SIZE = LogModule::MAX_TEXT + LogModule::MAX_NAME + 32U;

        void write(const LogRecord &record) override
        {
            static constexpr std::uint64_t US_PER_MS = 1000U;
            char line[LINE_SIZE];
            static_cast<void>(
                std::snprintf(line,
                              sizeof(line),
                              "%lu %s %s: %s\n",
                              static_cast<unsigned long>(record.timestampUs / US_PER_MS),
                              logLevelName(record.level),
                              record.moduleName,
                              record.text));
            rttWrite(line);
        }
    };
} // namespace mark4
