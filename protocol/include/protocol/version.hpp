#pragma once

/// @file
/// @brief Wire protocol version.

#include <cstdint>

namespace mark4
{
    /// First byte of every packet, checked by every consumer.
    inline constexpr std::uint8_t PROTOCOL_VERSION = 1U;
} // namespace mark4
