#pragma once

/// @file
/// @brief Blackbox log sink streaming over the telemetry link.

#include <cstddef>
#include <cstdint>

#include "platform/log_sink.hpp"
#include "platform/telemetry_sender.hpp"

namespace mark4
{
    /// Streams blackbox records to the PC over the telemetry link: no
    /// storage on the board, each record is queued as one serial frame,
    /// interleaved with the telemetry packets. A receiver tells the two
    /// streams apart by payload size and leading version byte. A record
    /// that does not fit the transmit ring is dropped whole by the sender.
    class LogSinkUart final : public AbsLogSink
    {
      public:
        /// @param sender link the records are queued on, shared with the
        ///        telemetry packets and owned by the caller
        explicit LogSinkUart(AbsTelemetrySender &sender)
            : m_sender(sender)
        {
        }

        /// @brief Queues one record on the link, never blocking.
        /// @param data record bytes
        /// @param size record size in bytes
        void write(const std::uint8_t *data, std::size_t size) override
        {
            m_sender.send(data, size);
        }

      private:
        AbsTelemetrySender &m_sender; ///< shared link, owned by the composition root
    };
} // namespace mark4
