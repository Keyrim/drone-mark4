#pragma once

/// @file
/// @brief The one place an Envelope meets the transport: encode on the
///        stack, then hand the bytes to a node id. Every answer a
///        composition emits (status, tuning, run stats, updater replies,
///        telemetry samples) goes through here.

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol/envelope.hpp"
#include "transport/transport.hpp"

namespace mark4
{
    /// @brief Encodes and sends one Envelope. Best effort: an envelope that
    ///        does not encode is dropped, and a frame no link took is
    ///        counted by the transport, never retried.
    /// @param transport transport the bytes leave by, not owned
    /// @param dst node to reach, BROADCAST_NODE for every node
    /// @param envelope message to send
    /// @return true when the frame left on every link it was meant for
    inline bool sendEnvelope(Transport &transport,
                             std::uint32_t dst,
                             const mark4_Envelope &envelope)
    {
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeEnvelope(envelope, bytes.data(), bytes.size(), size))
        {
            return false;
        }
        return transport.send(dst, bytes.data(), size);
    }
} // namespace mark4
