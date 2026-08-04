#pragma once

/// @file
/// @brief Counting telemetry sender for the sim variant.

#include <cstddef>
#include <cstdint>

#include "platform/telemetry_sender.hpp"

namespace mark4
{
    /// Counts what would be emitted (UDP broadcast later). No network yet.
    class TelemetrySenderSim final : public AbsTelemetrySender
    {
      public:
        void send(const std::uint8_t *data, std::size_t size) override;

        /// @return number of packets sent since construction
        [[nodiscard]] std::uint32_t packetCount() const
        {
            return m_packetCount;
        }

        /// @return total bytes sent since construction
        [[nodiscard]] std::size_t byteCount() const
        {
            return m_byteCount;
        }

      private:
        std::uint32_t m_packetCount = 0U;
        std::size_t m_byteCount = 0U;
    };
} // namespace mark4
