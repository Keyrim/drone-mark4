#pragma once

/// @file
/// @brief Debug console over SEGGER RTT: the probe reads a ring buffer in
///        RAM through the debug port, no UART pin involved. The control
///        block is hand-written against the RTT layout; any RTT-aware host
///        (JLinkRTTClient, gdb server) finds it by scanning RAM for its id.
///        Lines reach it through RttSink (rtt_sink.hpp), a sink of the log
///        library; nothing else writes here.

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
} // namespace mark4
