#pragma once

/// @file
/// @brief Wire protocol version.

#include <cstdint>

namespace mark4
{
    /// First byte of every packet, checked by every consumer; the packet
    /// type byte (protocol/header.hpp) follows it.
    inline constexpr std::uint8_t PROTOCOL_VERSION = 12U;
} // namespace mark4
