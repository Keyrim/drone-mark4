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
        /// @param[out] srcOut node the packet came from, valid only when
        ///        returning > 0: a service that answers one requester
        ///        instead of the whole bench needs it
        /// @return packet size in bytes, or 0 when nothing is pending
        virtual std::size_t poll(std::uint8_t *bufferOut,
                                 std::size_t capacity,
                                 std::uint32_t &srcOut) = 0;
    };
} // namespace mark4
