#pragma once

/// @file
/// @brief The one place an Envelope meets a telemetry sender: encode, then
///        hand the bytes over. Every answer a composition emits (telemetry,
///        tuning, run stats, updater replies) goes through here.

#include <array>
#include <cstddef>
#include <cstdint>

#include "platform/telemetry_sender.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// @brief Encodes and sends one Envelope. Best effort: an envelope that
    ///        does not encode is dropped, and the sender decides what a
    ///        failed send means.
    /// @param sender link the bytes go out on
    /// @param envelope message to send
    /// @return true when the bytes were handed to the sender
    inline bool sendEnvelope(AbsTelemetrySender &sender, const mark4_Envelope &envelope)
    {
        std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
        std::size_t size = 0U;
        if (!encodeEnvelope(envelope, bytes.data(), bytes.size(), size))
        {
            return false;
        }
        sender.send(bytes.data(), size);
        return true;
    }
} // namespace mark4
