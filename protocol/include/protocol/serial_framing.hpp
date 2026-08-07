#pragma once

/// @file
/// @brief Stream framing for protocol/ packets over serial links. UDP
///        preserves datagram boundaries, a UART does not: each packet
///        travels as SYNC0 SYNC1 length payload checksum, and a receiver
///        can resynchronize on the sync pair after any byte loss.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    inline constexpr std::uint8_t SERIAL_SYNC0 = 0xA5U;
    inline constexpr std::uint8_t SERIAL_SYNC1 = 0x5AU;

    /// Sync pair + length byte + trailing checksum.
    inline constexpr std::size_t SERIAL_FRAME_OVERHEAD = 4U;

    /// The length travels in one byte.
    inline constexpr std::size_t SERIAL_MAX_PAYLOAD = 255U;

    /// @brief XOR checksum of a payload.
    /// @param data payload bytes
    /// @param size payload size in bytes
    /// @return XOR of every byte
    constexpr std::uint8_t serialChecksum(const std::uint8_t *data, std::size_t size)
    {
        std::uint8_t checksum = 0U;
        for (std::size_t index = 0U; index < size; ++index)
        {
            checksum = checksum ^ data[index];
        }
        return checksum;
    }

    /// @brief Encodes one frame around a payload.
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
        out[3U + size] = serialChecksum(payload, size);
        return size + SERIAL_FRAME_OVERHEAD;
    }

    /// Incremental frame decoder: feed the stream byte by byte, complete
    /// payloads come out. Garbage between frames is skipped by hunting for
    /// the sync pair; a checksum mismatch drops the frame silently.
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
                    m_state = State::PAYLOAD;
                    break;
                case State::PAYLOAD:
                    m_payload[m_received] = byte;
                    ++m_received;
                    if (m_received == m_expected)
                    {
                        m_state = State::CHECKSUM;
                    }
                    break;
                case State::CHECKSUM:
                    m_state = State::SYNC0;
                    if (byte == serialChecksum(m_payload, m_expected))
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
            CHECKSUM, ///< payload complete, next byte is the checksum
        };

        std::uint8_t m_payload[SERIAL_MAX_PAYLOAD] = {}; ///< frame being decoded
        std::size_t m_expected = 0U;                     ///< announced payload size
        std::size_t m_received = 0U;                     ///< payload bytes accumulated
        State m_state = State::SYNC0;                    ///< decoder position
    };
} // namespace mark4
