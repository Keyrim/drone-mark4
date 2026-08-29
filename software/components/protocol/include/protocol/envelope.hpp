#pragma once

/// @file
/// @brief The one wire message and its codec. Every datagram and every
///        serial frame of the project carries exactly one mark4_Envelope,
///        the nanopb struct generated from mark4.proto; these two functions
///        are the only way bytes become one and back. No allocation, no
///        streams: a buffer in, a buffer out.

#include <cstddef>
#include <cstdint>

#include <pb.h>

#include "mark4.pb.h"
#include "protocol/wire_hash.hpp"

namespace mark4
{
    /// Largest encoded Envelope, every bounded field at its maximum.
    inline constexpr std::size_t MAX_ENVELOPE_SIZE = mark4_Envelope_size;

    /// Stack budget of the decoded struct: it lives on the stack of a 500 Hz
    /// loop and inside the command rings. The oneof is a union, so the
    /// struct is as large as its largest member.
    inline constexpr std::size_t MAX_ENVELOPE_STRUCT_SIZE = 400U;

    static_assert(sizeof(mark4_Envelope) < MAX_ENVELOPE_STRUCT_SIZE,
                  "the Envelope struct grew past its stack budget");

    /// Bits a field number is shifted by in a protobuf tag byte.
    inline constexpr unsigned PB_TAG_FIELD_SHIFT = 3U;

    /// Highest field number whose tag is a single byte.
    inline constexpr unsigned PB_SINGLE_BYTE_TAG_MAX = 15U;

    static_assert(mark4_Envelope_announce_tag <= PB_SINGLE_BYTE_TAG_MAX,
                  "the announce tag must fit one byte for the relay filter");

    /// First byte of every encoded Envelope carrying an Announce: the
    /// Envelope has one field, its oneof, so the body's tag opens the bytes.
    inline constexpr std::uint8_t ANNOUNCE_TAG_BYTE = static_cast<std::uint8_t>(
        (mark4_Envelope_announce_tag << PB_TAG_FIELD_SHIFT) | PB_WT_STRING);

    /// @brief Says whether encoded bytes carry an Announce, without decoding
    ///        them: one byte compared. What a relay asks of every broadcast
    ///        before letting it onto a slow link.
    /// @param data encoded Envelope
    /// @param size byte count
    /// @return true when the body is an Announce
    constexpr bool envelopeIsAnnounce(const std::uint8_t *data, std::size_t size)
    {
        return data != nullptr && size > 0U && data[0] == ANNOUNCE_TAG_BYTE;
    }

    /// @brief Encodes one Envelope.
    /// @param envelope message to encode; which_body must name a body
    /// @param[out] out destination bytes
    /// @param capacity bytes available in out
    /// @param[out] sizeOut bytes written, valid only when returning true
    /// @return false when the message does not fit or is malformed
    bool encodeEnvelope(const mark4_Envelope &envelope,
                        std::uint8_t *out,
                        std::size_t capacity,
                        std::size_t &sizeOut);

    /// @brief Decodes one Envelope.
    /// @param data encoded bytes
    /// @param size byte count
    /// @param[out] envelopeOut decoded message, zeroed first; which_body is 0
    ///             when the bytes decoded but carried no body
    /// @return false when the bytes are not a valid Envelope
    bool decodeEnvelope(const std::uint8_t *data, std::size_t size, mark4_Envelope &envelopeOut);
} // namespace mark4
