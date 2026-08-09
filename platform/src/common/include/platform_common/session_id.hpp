#pragma once

/// @file
/// @brief Identity of one process start, the value an announce carries so a
///        consumer tells a restart from a refresh behind unchanged ports.

#include <chrono>
#include <cstdint>

#include <unistd.h>

namespace mark4
{
    /// Odd 32-bit multiplier (Knuth) spreading a process id over the whole
    /// word, so two instances started in the same microsecond still differ.
    inline constexpr std::uint32_t SESSION_ID_MIX = 2654435761U;

    /// @brief Draws the identity of this process start. No random_device and
    ///        no exceptions: a process id and the monotonic clock already
    ///        separate two instances of a batch campaign, which is all a
    ///        consumer needs to tell a restart from a refresh.
    /// @return the session identity, never 0 (0 means "assigns none")
    inline std::uint32_t makeSessionId()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        const std::uint32_t mixed = (static_cast<std::uint32_t>(::getpid()) * SESSION_ID_MIX) ^
                                    static_cast<std::uint32_t>(ticks);
        return mixed == 0U ? 1U : mixed;
    }
} // namespace mark4
