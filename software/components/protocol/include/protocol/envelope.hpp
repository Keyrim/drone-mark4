#pragma once

/// @file
/// @brief The one wire message and its codec. Every datagram and every
///        serial frame of the project carries exactly one mark4_Envelope,
///        the nanopb struct generated from mark4.proto; these two functions
///        are the only way bytes become one and back. No allocation, no
///        streams: a buffer in, a buffer out.

#include <cstddef>
#include <cstdint>

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
