#pragma once

/// @file
/// @brief Blackbox log output.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Persists blackbox records (SD/flash on target, file on desktop).
    class AbsLogSink
    {
      public:
        virtual ~AbsLogSink() = default;

        /// @brief Appends one record.
        /// @param data record bytes
        /// @param size record size in bytes
        virtual void write(const std::uint8_t *data, std::size_t size) = 0;
    };
} // namespace mark4
