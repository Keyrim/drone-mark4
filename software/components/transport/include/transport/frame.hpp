#pragma once

/// @file
/// @brief Frame header the transport puts in front of every payload:
///        src u32, dst u32, seq u16, hops u8, little-endian, 11 bytes. The
///        payload behind it is opaque; a medium that does not preserve
///        datagram boundaries adds its own length (see UartLink).

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace mark4
{
    /// Destination meaning "every node", on every link.
    inline constexpr std::uint32_t BROADCAST_NODE = 0U;

    /// Bytes of the header in front of every payload.
    inline constexpr std::size_t FRAME_HEADER_SIZE = 11U;

    /// Largest payload one frame carries. The largest packet of the project
    /// today is the OTA chunk (251 bytes), so this leaves room to grow.
    inline constexpr std::size_t MAX_PAYLOAD = 512U;

    /// Largest frame a link has to carry or accept.
    inline constexpr std::size_t MAX_FRAME_SIZE = FRAME_HEADER_SIZE + MAX_PAYLOAD;

    /// What every frame opens with.
    struct FrameHeader
    {
        std::uint32_t src = 0U; ///< node that produced the payload
        std::uint32_t dst = 0U; ///< node it is for, BROADCAST_NODE for all
        std::uint16_t seq = 0U; ///< per-sender counter, wraps
        std::uint8_t hops = 0U; ///< relays left; a relay decrements and drops at 0
    };

    /// @brief Writes one header, little-endian whatever the host order.
    /// @param header header to write
    /// @param[out] out receives FRAME_HEADER_SIZE bytes
    constexpr void encodeFrameHeader(const FrameHeader &header, std::uint8_t *out)
    {
        std::size_t index = 0U;
        for (const std::uint32_t word : {header.src, header.dst})
        {
            for (unsigned shift = 0U; shift < 32U; shift += 8U)
            {
                out[index] = static_cast<std::uint8_t>(word >> shift);
                ++index;
            }
        }
        out[index] = static_cast<std::uint8_t>(header.seq);
        out[index + 1U] = static_cast<std::uint8_t>(header.seq >> 8U);
        out[index + 2U] = header.hops;
    }

    /// @brief Reads one header.
    /// @param data frame bytes
    /// @param size frame size in bytes
    /// @param[out] headerOut receives the header when the frame holds one
    /// @return false when the frame is shorter than a header
    constexpr bool decodeFrameHeader(const std::uint8_t *data,
                                     std::size_t size,
                                     FrameHeader &headerOut)
    {
        if (size < FRAME_HEADER_SIZE)
        {
            return false;
        }
        std::uint32_t words[2] = {0U, 0U};
        std::size_t index = 0U;
        for (std::uint32_t &word : words)
        {
            for (unsigned shift = 0U; shift < 32U; shift += 8U)
            {
                word |= static_cast<std::uint32_t>(data[index]) << shift;
                ++index;
            }
        }
        headerOut.src = words[0];
        headerOut.dst = words[1];
        headerOut.seq = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[index]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[index + 1U]) << 8U));
        headerOut.hops = data[index + 2U];
        return true;
    }
} // namespace mark4
