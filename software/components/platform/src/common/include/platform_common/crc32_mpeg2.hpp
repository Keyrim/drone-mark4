#pragma once

/// @file
/// @brief Software CRC-32/MPEG-2, the one image and metadata checksum of
///        the update system (see protocol/ota.hpp): polynomial
///        0x04C11DB7, init 0xFFFFFFFF, no reflection, no final xor,
///        consumed as 32-bit words packed in memory order (byte 0 is the
///        word's least significant byte). It reproduces bit for bit
///        what the F405 hardware CRC unit computes over the same words;
///        the stm32 store may use either, everything else uses this.

#include <cstddef>
#include <cstdint>

#include "protocol/ota.hpp"

namespace mark4
{
    /// Streaming CRC-32/MPEG-2 over an arbitrary byte sequence. Bytes are
    /// packed into 32-bit words in memory order (little-endian storage,
    /// so byte 0 is the word's least significant byte, exactly what the
    /// hardware unit sees when fed words read from flash); finish() pads
    /// the tail to a full word with 0xFF, per the image CRC convention.
    class Crc32Mpeg2
    {
      public:
        /// @brief Feeds bytes; callable any number of times.
        /// @param data bytes to consume
        /// @param size byte count
        void update(const std::uint8_t *data, std::size_t size)
        {
            for (std::size_t i = 0U; i < size; ++i)
            {
                m_word |= static_cast<std::uint32_t>(data[i]) << (BITS_PER_BYTE * m_pending);
                ++m_pending;
                if (m_pending == BYTES_PER_WORD)
                {
                    consumeWord();
                }
            }
        }

        /// @brief Pads the tail with 0xFF to a word boundary and returns
        ///        the CRC. The object is spent afterwards.
        std::uint32_t finish()
        {
            while (m_pending != 0U)
            {
                const std::uint8_t pad = 0xFFU;
                update(&pad, 1U);
            }
            return m_crc;
        }

      private:
        static constexpr std::uint32_t POLYNOMIAL = 0x04C11DB7U; ///< CRC-32/MPEG-2
        static constexpr std::uint32_t TOP_BIT = 0x80000000U;    ///< bit shifted out each round
        static constexpr std::size_t BITS_PER_BYTE = 8U;         ///< byte to word packing shift
        static constexpr std::size_t BITS_PER_WORD = 32U;        ///< rounds per consumed word
        static constexpr std::size_t BYTES_PER_WORD = 4U;        ///< bytes gathered per word

        void consumeWord()
        {
            m_crc ^= m_word;
            for (std::size_t bit = 0U; bit < BITS_PER_WORD; ++bit)
            {
                const bool top = (m_crc & TOP_BIT) != 0U;
                m_crc <<= 1U;
                if (top)
                {
                    m_crc ^= POLYNOMIAL;
                }
            }
            m_word = 0U;
            m_pending = 0U;
        }

        std::uint32_t m_crc = 0xFFFFFFFFU; ///< running register
        std::uint32_t m_word = 0U;         ///< bytes gathered toward the next word
        std::size_t m_pending = 0U;        ///< bytes gathered so far, 0 to 3
    };

    /// @brief One-shot CRC-32/MPEG-2 of a buffer, tail padded with 0xFF.
    /// @param data bytes to checksum
    /// @param size byte count
    inline std::uint32_t crc32Mpeg2(const std::uint8_t *data, std::size_t size)
    {
        Crc32Mpeg2 crc;
        crc.update(data, size);
        return crc.finish();
    }
} // namespace mark4
