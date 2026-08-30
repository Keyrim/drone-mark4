#pragma once

/// @file
/// @brief Sink for a desktop process: one line per record on stdout,
///        "HH:MM:SS.mmm LEVL module: text", flushed so a piped console
///        (an editor's debug console) shows it live.

#include "log/sink.hpp"

namespace mark4
{
    class ConsoleSinkPosix final : public AbsLogSink
    {
      public:
        void write(const LogRecord &record) override;
    };
} // namespace mark4
