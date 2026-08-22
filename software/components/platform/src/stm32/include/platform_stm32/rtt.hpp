#pragma once

/// @file
/// @brief Debug console over SEGGER RTT: the probe reads a ring buffer in
///        RAM through the debug port, no UART pin involved. The control
///        block is hand-written against the RTT layout; any RTT-aware host
///        (JLinkRTTClient, gdb server) finds it by scanning RAM for its id.

#include <cstddef>

namespace mark4
{
    /// @brief Prepares the RTT control block. Must run before the first
    ///        write; safe to call with the probe attached or not.
    void rttInit();

    /// @brief Queues a zero-terminated string on the up buffer. Never
    ///        blocks: when the host has not drained enough space, the whole
    ///        write is dropped so the caller keeps its timing.
    /// @param text string to queue, not kept after return
    void rttWrite(const char *text);

    /// @brief printf-style convenience over rttWrite(). Formats into a
    ///        fixed stack buffer of RTT_PRINTF_SIZE bytes; longer output is
    ///        truncated. Integer conversions only (newlib-nano, no float).
    /// @param format printf format string
    void rttPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));

    /// Stack formatting buffer of rttPrintf(), truncation threshold.
    inline constexpr std::size_t RTT_PRINTF_SIZE = 128U;
} // namespace mark4
