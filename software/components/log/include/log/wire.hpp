#pragma once

/// @file
/// @brief The log library on the wire: the sink that broadcasts every line
///        as a mark4.Log envelope, the node's module table as mark4.LogModules
///        pages, and the LogControl a client drives the levels with. Target
///        `log_wire`: the one part of the library that links protocol/.

#include <cstddef>
#include <cstdint>

#include "log/sink.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Sends one encoded Envelope; how (broadcast, unicast, a mirror to a
    /// websocket) is the application's. Returns false when it did not go out.
    using LogSendFn = bool (*)(void *context, const std::uint8_t *data, std::size_t size);

    class TransportSink final : public AbsLogSink
    {
      public:
        /// Lines let through per second; the rest are dropped and counted.
        static constexpr std::uint32_t MAX_LINES_PER_SECOND = 50U;

        /// Window of the rate limit [us].
        static constexpr std::uint64_t WINDOW_US = 1'000'000U;

        /// @param send route the encoded envelopes take
        /// @param context handed back to send
        TransportSink(LogSendFn send, void *context)
            : m_send(send),
              m_context(context)
        {
        }

        /// @brief Encodes and sends the record. Past the rate limit the line
        ///        is counted instead; the count goes out once per second as
        ///        a WARN of log/core, directly, so it never rate limits
        ///        itself.
        void write(const LogRecord &record) override;

        /// @return lines dropped by the rate limit, cumulative
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        /// @brief Encodes one record as a Log envelope and sends it.
        void send(const LogRecord &record);

        LogSendFn m_send;                   ///< output route
        void *m_context;                    ///< its context
        std::uint64_t m_windowStartUs = 0U; ///< start of the current second
        std::uint32_t m_windowCount = 0U;   ///< lines sent in it
        std::uint32_t m_windowDropped = 0U; ///< lines refused in it
        std::uint32_t m_dropped = 0U;       ///< lines refused, cumulative
    };

    /// @brief Sends the module table as LogModules pages, PAGE_SIZE modules
    ///        each: 48 names of 32 characters do not fit one frame.
    /// @param send route
    /// @param context handed back to send
    /// @return false when a page did not go out
    bool logPublishModules(LogSendFn send, void *context);

    /// @brief Carries out one LogControl: a query asks for the table, a set
    ///        moves one module's level.
    /// @param control the request
    /// @return true when the table must be published again (a query, or a
    ///         level that changed)
    bool logHandleControl(const mark4_LogControl &control);

    /// @return the wire level of a library level
    constexpr mark4_LogLevel logLevelToWire(LogLevel level)
    {
        return static_cast<mark4_LogLevel>(level);
    }

    /// @brief Reads a wire level.
    /// @param wire value received
    /// @param[out] levelOut the level, untouched when the value is unknown
    /// @return false when the value names no level of this build
    bool logLevelFromWire(mark4_LogLevel wire, LogLevel &levelOut);
} // namespace mark4
