#pragma once

/// @file
/// @brief Telemetry output.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Sends raw protocol/ packets to the telemetry link (UART, UDP...).
    class AbsTelemetrySender
    {
      public:
        virtual ~AbsTelemetrySender() = default;

        /// @brief Sends one packet.
        /// @param data packet bytes
        /// @param size packet size in bytes
        virtual void send(const std::uint8_t *data, std::size_t size) = 0;
    };
} // namespace mark4
