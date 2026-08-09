#pragma once

/// @file
/// @brief Stream framing for protocol/ packets over serial links. UDP
///        preserves datagram boundaries, a UART does not: each packet
///        travels as SYNC0 SYNC1 length payload crc16, and a receiver
///        can resynchronize on the sync pair after any byte loss. The
///        CRC covers the length byte and the payload, so a corrupted
///        length cannot swallow the stream or synthesize a plausible
///        frame; XOR-class checksums are not enough for the blackbox
///        stream, which is the forensic record.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    inline constexpr std::uint8_t SERIAL_SYNC0 = 0xA5U;
    inline constexpr std::uint8_t SERIAL_SYNC1 = 0x5AU;

    /// Sync pair + length byte + trailing CRC-16.
    inline constexpr std::size_t SERIAL_FRAME_OVERHEAD = 5U;

    /// The length travels in one byte.
    inline constexpr std::size_t SERIAL_MAX_PAYLOAD = 255U;

    /// Initial value of the CRC-16 computation.
    inline constexpr std::uint16_t CRC16_INIT = 0xFFFFU;

    /// CRC-16/CCITT-FALSE generator polynomial.
    inline constexpr std::uint16_t CRC16_POLYNOMIAL = 0x1021U;

    /// Most significant bit of the 16-bit CRC register.
    inline constexpr std::uint16_t CRC16_MSB = 0x8000U;

    /// Bits shifted to move a byte into the high half of the CRC register.
    inline constexpr unsigned CRC16_BYTE_SHIFT = 8U;

    /// Mask extracting the low byte of the CRC for the wire.
    inline constexpr std::uint16_t CRC16_LOW_MASK = 0xFFU;

    /// @brief Feeds bytes into a running CRC-16/CCITT-FALSE computation
    ///        (polynomial 0x1021, no reflection, no final xor) - the one
    ///        CRC of the project, shared by the serial framing and the
    ///        blackbox records.
    /// @param crc running value, CRC16_INIT to start a computation
    /// @param data bytes to feed
    /// @param size number of bytes to feed
    /// @return updated running value
    constexpr std::uint16_t crc16(std::uint16_t crc, const std::uint8_t *data, std::size_t size)
    {
        for (std::size_t index = 0U; index < size; ++index)
        {
            crc = static_cast<std::uint16_t>(crc ^ static_cast<std::uint16_t>(data[index])
                                                       << CRC16_BYTE_SHIFT);
            for (unsigned bit = 0U; bit < CRC16_BYTE_SHIFT; ++bit)
            {
                crc = (crc & CRC16_MSB) != 0U
                          ? static_cast<std::uint16_t>((crc << 1U) ^ CRC16_POLYNOMIAL)
                          : static_cast<std::uint16_t>(crc << 1U);
            }
        }
        return crc;
    }

    /// @brief Encodes one frame around a payload. The CRC covers the
    ///        length byte and the payload, and travels little-endian.
    /// @param payload packet bytes
    /// @param size packet size, at most SERIAL_MAX_PAYLOAD
    /// @param out receives the frame; must hold size + SERIAL_FRAME_OVERHEAD
    /// @return bytes written to out, 0 when the payload is too large or empty
    inline std::size_t encodeSerialFrame(const std::uint8_t *payload,
                                         std::size_t size,
                                         std::uint8_t *out)
    {
        if (size == 0U || size > SERIAL_MAX_PAYLOAD)
        {
            return 0U;
        }
        out[0] = SERIAL_SYNC0;
        out[1] = SERIAL_SYNC1;
        out[2] = static_cast<std::uint8_t>(size);
        for (std::size_t index = 0U; index < size; ++index)
        {
            out[3U + index] = payload[index];
        }
        const std::uint16_t crc = crc16(CRC16_INIT, &out[2], size + 1U);
        out[3U + size] = static_cast<std::uint8_t>(crc & CRC16_LOW_MASK);
        out[4U + size] = static_cast<std::uint8_t>(crc >> CRC16_BYTE_SHIFT);
        return size + SERIAL_FRAME_OVERHEAD;
    }

    /// Incremental frame decoder: feed the stream byte by byte, complete
    /// payloads come out. Garbage between frames is skipped by hunting for
    /// the sync pair; a CRC mismatch drops the frame silently.
    class SerialFrameParser
    {
      public:
        /// @brief Consumes one stream byte.
        /// @param byte next byte of the stream
        /// @return payload size when this byte completed a valid frame,
        ///         0 otherwise; the payload is then readable via payload()
        ///         until the next feed()
        std::size_t feed(std::uint8_t byte)
        {
            switch (m_state)
            {
                case State::SYNC0:
                    m_state = (byte == SERIAL_SYNC0) ? State::SYNC1 : State::SYNC0;
                    break;
                case State::SYNC1:
                    // A repeated SYNC0 keeps the first byte of the pair valid.
                    m_state = (byte == SERIAL_SYNC1)   ? State::LENGTH
                              : (byte == SERIAL_SYNC0) ? State::SYNC1
                                                       : State::SYNC0;
                    break;
                case State::LENGTH:
                    if (byte == 0U)
                    {
                        m_state = State::SYNC0;
                        break;
                    }
                    m_expected = byte;
                    m_received = 0U;
                    m_crc = crc16(CRC16_INIT, &byte, 1U);
                    m_state = State::PAYLOAD;
                    break;
                case State::PAYLOAD:
                    m_payload[m_received] = byte;
                    ++m_received;
                    m_crc = crc16(m_crc, &byte, 1U);
                    if (m_received == m_expected)
                    {
                        m_state = State::CRC_LOW;
                    }
                    break;
                case State::CRC_LOW:
                    m_crcLow = byte;
                    m_state = State::CRC_HIGH;
                    break;
                case State::CRC_HIGH:
                    m_state = State::SYNC0;
                    if (m_crc == static_cast<std::uint16_t>(static_cast<std::uint16_t>(byte)
                                                                << CRC16_BYTE_SHIFT |
                                                            m_crcLow))
                    {
                        return m_expected;
                    }
                    break;
            }
            return 0U;
        }

        /// @return payload of the last completed frame
        [[nodiscard]] const std::uint8_t *payload() const
        {
            return m_payload;
        }

      private:
        /// Position in the frame being decoded.
        enum class State : std::uint8_t
        {
            SYNC0,    ///< hunting for the first sync byte
            SYNC1,    ///< first sync byte seen
            LENGTH,   ///< sync pair seen, next byte is the length
            PAYLOAD,  ///< accumulating payload bytes
            CRC_LOW,  ///< payload complete, next byte is the CRC low byte
            CRC_HIGH, ///< next byte is the CRC high byte
        };

        std::uint8_t m_payload[SERIAL_MAX_PAYLOAD] = {}; ///< frame being decoded
        std::size_t m_expected = 0U;                     ///< announced payload size
        std::size_t m_received = 0U;                     ///< payload bytes accumulated
        std::uint16_t m_crc = CRC16_INIT;                ///< running CRC over length + payload
        std::uint8_t m_crcLow = 0U;                      ///< first CRC byte of the frame
        State m_state = State::SYNC0;                    ///< decoder position
    };
} // namespace mark4
