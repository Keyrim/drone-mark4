#pragma once

/// @file
/// @brief What the updater asks of the node before it lets a session in: the
///        two facts a node knows about itself and the update brick does not.

namespace mark4
{
    /// What the updater asks of the node before it lets a session in.
    class AbsOtaGate
    {
      public:
        /// Inline like every other abstract class of the components: the
        /// libraries are built without RTTI and an out-of-line destructor
        /// would leave the typeinfo the RTTI-enabled executables reference
        /// undefined.
        virtual ~AbsOtaGate() = default;

        /// @return true while the node may spin motors: no update then
        [[nodiscard]] virtual bool armed() const = 0;

        /// @return true while the pack is above the update floor
        [[nodiscard]] virtual bool voltageOk() const = 0;
    };
} // namespace mark4
