#pragma once

/// @file
/// @brief An in-memory UART: a byte pipe with one direction each way and a
///        stream end for each side, so two UartLinks talk through it the
///        way the board and the ESP32 relay talk over their wire.

#include <cstddef>
#include <cstdint>
#include <deque>

#include "transport/uart_link.hpp"

namespace mark4
{
    /// Byte pipe with a separate direction each way: what one end writes
    /// the other end reads. Allocates freely: this is a test.
    class BytePipe
    {
      public:
        std::deque<std::uint8_t> toB; ///< bytes written by A, read by B
        std::deque<std::uint8_t> toA; ///< bytes written by B, read by A
    };

    /// One end of a BytePipe.
    class PipeEnd final : public AbsByteStream
    {
      public:
        /// @param in bytes this end reads
        /// @param out bytes this end writes
        PipeEnd(std::deque<std::uint8_t> &in, std::deque<std::uint8_t> &out)
            : m_in(in),
              m_out(out)
        {
        }

        std::size_t read(std::uint8_t *bufferOut, std::size_t capacity) override
        {
            std::size_t count = 0U;
            while (count < capacity && !m_in.empty())
            {
                bufferOut[count] = m_in.front();
                m_in.pop_front();
                ++count;
            }
            return count;
        }

        bool write(const std::uint8_t *data, std::size_t size) override
        {
            m_out.insert(m_out.end(), data, data + size);
            m_written += size;
            return true;
        }

        /// @return bytes written into the pipe by this end so far
        [[nodiscard]] std::size_t written() const
        {
            return m_written;
        }

      private:
        std::deque<std::uint8_t> &m_in;  ///< read side
        std::deque<std::uint8_t> &m_out; ///< write side
        std::size_t m_written = 0U;      ///< bytes pushed through write()
    };
} // namespace mark4
