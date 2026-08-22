#pragma once

/// @file
/// @brief Polled command input.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Receives raw protocol/ packets, polled once per tick, never blocking.
    /// Decoding belongs to the consumer.
    class AbsCommandReceiver
    {
      public:
        virtual ~AbsCommandReceiver() = default;

        /// @brief Copies the next pending packet, if any.
        /// @param[out] bufferOut destination, valid only when returning > 0
        /// @param capacity size of the destination buffer in bytes
        /// @return packet size in bytes, or 0 when nothing is pending
        virtual std::size_t poll(std::uint8_t *bufferOut, std::size_t capacity) = 0;
    };
} // namespace mark4
