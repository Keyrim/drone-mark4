#pragma once

/// @file
/// @brief Copying single fields in and out of a byte-packed wire struct.
///
/// The structs in protocol/ are packed to the byte, so a field of theirs sits
/// wherever the layout puts it, aligned or not. Reading or writing one
/// through the member itself is fine, the compiler knows the layout; binding
/// a reference to it is not, and that is exactly what handing it to a
/// function or to a JSON library does. Scalars can simply be copied to a
/// local, but a field of class type (an std::array of floats, on this wire)
/// copies through its own constructor, which takes that forbidden reference.
/// These two copy the bytes instead.

#include <cstring>

namespace mark4
{
    /// @brief Copies a field out of a packed struct into an aligned value.
    /// @tparam T type of the field
    /// @param field address of the field
    /// @return an aligned copy of it
    template <typename T> T readPackedField(const T *field)
    {
        T value{};
        std::memcpy(&value, field, sizeof(T));
        return value;
    }

    /// @brief Copies an aligned value into a field of a packed struct.
    /// @tparam T type of the field
    /// @param field address of the field
    /// @param value value to store
    template <typename T> void writePackedField(T *field, const T &value)
    {
        std::memcpy(field, &value, sizeof(T));
    }
} // namespace mark4
