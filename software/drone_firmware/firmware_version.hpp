#pragma once

/// @file
/// @brief The firmware version this build announces: stamped into the OTA
///        image header (image_header.cpp) so the bootloader, the packaging
///        script's manifest and the hub all read the same three numbers.
///        Bump it by hand, semantic-version style: patch for a fix, minor
///        for behaviour worth naming on the bench, major for a break in
///        what the ground side may assume.

#include <cstdint>

namespace mark4
{
    inline constexpr std::uint8_t FIRMWARE_VERSION_MAJOR = 0U;
    inline constexpr std::uint8_t FIRMWARE_VERSION_MINOR = 1U;
    inline constexpr std::uint8_t FIRMWARE_VERSION_PATCH = 3U;
} // namespace mark4
