#pragma once

/// @file
/// @brief The hardware random number generator of the F405, and the one
///        thing this board draws from it: the boot id of its transport.

#include <cstdint>

namespace mark4
{
    /// @brief Draws the identity of this run of this board from the RNG
    ///        peripheral. Never derived from the MCU unique id, which is
    ///        what makes the node id stable: this one has to differ from one
    ///        boot to the next. When the peripheral never reports a value it
    ///        falls back on the cycle counter mixed with the unique id, which
    ///        is enough for a number nothing but a comparison is done with.
    /// @return the boot id, never 0
    std::uint32_t randomBootId();
} // namespace mark4
