#pragma once

/// @file
/// @brief The log library on the wire, without the wire itself: the level
///        codec and the two helpers that fill the messages of
///        `protocol/mark4.proto` from the module registry. Target
///        `log_wire`: the one part of the library that links protocol/.

#include <cstddef>
#include <cstdint>

#include "log/module.hpp"
#include "log/sink.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    /// Modules one LogModules page carries at most, the bound of
    /// mark4.LogModules.modules: 48 names of 32 characters do not fit one
    /// frame.
    inline constexpr std::size_t LOG_MODULES_PER_PAGE =
        sizeof(mark4_LogModules::modules) / sizeof(mark4_LogModuleInfo);

    /// @return the wire level of a library level
    constexpr mark4_LogLevel logLevelToWire(LogLevel level)
    {
        return static_cast<mark4_LogLevel>(level);
    }

    /// @brief Reads a wire level.
    /// @param wire value received
    /// @param[out] levelOut the level, untouched when the value is unknown
    /// @return false when the value names no level of this build
    bool logLevelFromWire(mark4_LogLevel wire, LogLevel &levelOut);

    /// @brief Describes one module as the wire does.
    /// @param module module to describe
    /// @param[out] infoOut message to fill, every field written
    void logFillModuleInfo(const LogModule &module, mark4_LogModuleInfo &infoOut);

    /// @brief Fills one page of the module table, walking the registry from
    ///        cursor. A cursor past the end yields an empty page carrying
    ///        the total, which is how a requester learns it asked too far.
    /// @param cursor index of the first module to describe
    /// @param[out] pageOut message to fill: total, cursor and up to
    ///             LOG_MODULES_PER_PAGE modules
    void logFillModulesPage(std::uint32_t cursor, mark4_LogModules &pageOut);
} // namespace mark4
