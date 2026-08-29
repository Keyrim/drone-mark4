#pragma once

/// @file
/// @brief Transport link over a byte stream (a UART, a pseudo-serial UDP
///        tunnel). The link owns no hardware: it is handed a byte source
///        and sink, and applies the serial framing (sync pair, length,
///        CRC-16) so frame boundaries survive a medium that has none.

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/link.hpp"
#include "transport/serial_framing.hpp"

namespace mark4
{
    /// The bytes behind a UartLink. Both calls are non-blocking.
    class AbsByteStream
    {
      public:
        virtual ~AbsByteStream() = default;

        /// @brief Takes the bytes pending on the stream.
        /// @param[out] bufferOut receives the bytes
        /// @param capacity size of bufferOut
        /// @return bytes read, 0 when nothing is pending
        virtual std::size_t read(std::uint8_t *bufferOut, std::size_t capacity) = 0;

        /// @brief Pushes bytes into the stream.
        /// @param data bytes to write
        /// @param size number of bytes
        /// @return true when every byte was accepted
        virtual bool write(const std::uint8_t *data, std::size_t size) = 0;
    };

    /// Point-to-point link: broadcast and unicast are the same send, and
    /// addresses carry nothing. One frame is at most SERIAL_MAX_PAYLOAD
    /// bytes, header included: the length travels in one byte.
    class UartLink final : public AbsLink
    {
      public:
        /// Bytes taken from the stream per read.
        static constexpr std::size_t READ_CHUNK = 64U;

        /// @param stream bytes in and out, owned by the composition root
        explicit UartLink(AbsByteStream &stream)
            : m_stream(stream)
        {
        }

        bool send(const std::uint8_t *data, std::size_t size, const LinkAddress &address) override;
        bool broadcast(const std::uint8_t *data, std::size_t size) override;
        std::size_t receive(std::uint8_t *bufferOut,
                            std::size_t capacity,
                            LinkAddress &fromOut) override;

      private:
        AbsByteStream &m_stream;                          ///< the medium, not owned
        SerialFrameParser m_parser;                       ///< incremental decoder
        std::array<std::uint8_t, READ_CHUNK> m_pending{}; ///< bytes read, not yet fed
        std::size_t m_pendingSize = 0U;                   ///< bytes in m_pending
        std::size_t m_pendingIndex = 0U;                  ///< next byte to feed
        std::array<std::uint8_t, SERIAL_MAX_PAYLOAD + SERIAL_FRAME_OVERHEAD>
            m_txFrame{}; ///< frame being sent
    };
} // namespace mark4
