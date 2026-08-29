#pragma once

/// @file
/// @brief Node identities: self-assigned at start, never configured. A
///        desktop process draws a random one; a board derives one from its
///        unique id or MAC with hashNodeId().

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// FNV-1a 32-bit offset basis.
    inline constexpr std::uint32_t FNV1A_OFFSET = 2166136261U;

    /// FNV-1a 32-bit prime.
    inline constexpr std::uint32_t FNV1A_PRIME = 16777619U;

    /// @brief Folds a hardware identity (MCU UID, MAC) into a node id.
    /// @param bytes identity bytes
    /// @param size number of bytes
    /// @return FNV-1a hash of the bytes, never 0 (0 is the broadcast node)
    constexpr std::uint32_t hashNodeId(const std::uint8_t *bytes, std::size_t size)
    {
        std::uint32_t hash = FNV1A_OFFSET;
        for (std::size_t index = 0U; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= FNV1A_PRIME;
        }
        return hash == 0U ? 1U : hash;
    }

    /// @brief Draws a node id from /dev/urandom. POSIX only.
    /// @return the id, never 0; 0 when the random source cannot be read
    std::uint32_t randomNodeId();
} // namespace mark4
