#pragma once

/// @file
/// @brief Console lines over the transport: one Log envelope per line,
///        broadcast so every ground tool reads it. Rate limited, because
///        a line is a diagnostic and the link carries the flight.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "platform_common/envelope_io.hpp"
#include "platform_common/telemetry_sender_transport.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    class LogSinkTransport
    {
      public:
        /// Lines let through per second; the rest are dropped and counted.
        static constexpr std::uint32_t MAX_LINES_PER_SECOND = 20U;

        /// Window of the rate limit [us].
        static constexpr std::uint64_t WINDOW_US = 1'000'000U;

        /// Longest text carried, the bound of mark4.Log.text.
        static constexpr std::size_t MAX_TEXT = sizeof(mark4_Log::text) - 1U;

        /// @param transport transport the lines leave by, owned by the
        ///        composition root
        explicit LogSinkTransport(Transport &transport)
            : m_sender(transport)
        {
        }

        /// @brief Broadcasts one line, truncated to MAX_TEXT.
        /// @param level severity
        /// @param text zero-terminated line
        /// @param nowUs current instant [us], the rate limit's clock
        void log(mark4_LogLevel level, const char *text, std::uint64_t nowUs)
        {
            if (nowUs - m_windowStartUs >= WINDOW_US)
            {
                m_windowStartUs = nowUs;
                m_windowCount = 0U;
            }
            if (m_windowCount >= MAX_LINES_PER_SECOND)
            {
                ++m_dropped;
                return;
            }
            ++m_windowCount;
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = mark4_Envelope_log_tag;
            envelope.body.log.timestamp_us = nowUs;
            envelope.body.log.level = level;
            std::strncpy(envelope.body.log.text, text, MAX_TEXT);
            static_cast<void>(sendEnvelope(m_sender, envelope));
        }

        /// @return lines dropped by the rate limit
        [[nodiscard]] std::uint32_t dropped() const
        {
            return m_dropped;
        }

      private:
        TelemetrySenderTransport m_sender;  ///< broadcast path
        std::uint64_t m_windowStartUs = 0U; ///< start of the current second
        std::uint32_t m_windowCount = 0U;   ///< lines sent in it
        std::uint32_t m_dropped = 0U;       ///< lines refused
    };
} // namespace mark4
