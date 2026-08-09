#pragma once

/// @file
/// @brief Named sets of tuning values, kept as plain JSON files in a
///        directory. A bench session that found a good set writes it down
///        under a name and pushes it back later; nothing else in the system
///        stores tuning at all, since the flight processes have no flash to
///        keep it in.
///
/// One file per profile rather than one file holding them all: profiles are
/// written by hand as often as by the hub, a diff of one is a diff of one,
/// and a file that got corrupted costs only its own profile.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mark4
{
    /// Values of one profile, parameter id to value. Ordered, so a saved
    /// file always comes out in the same order whatever the caller's.
    using TuningValues = std::map<std::uint16_t, float>;

    /// A directory of profile files. Owns no handle: every call opens what it
    /// needs and closes it, so nothing has to be kept in sync with the disk.
    class TuningProfiles
    {
      public:
        /// Version stamped in every file written, and the only one read.
        static constexpr int FORMAT_VERSION = 1;

        /// Longest accepted profile name.
        static constexpr std::size_t MAX_NAME_LENGTH = 32U;

        /// @param directory directory the profile files live in
        explicit TuningProfiles(std::string directory);

        /// @brief Checks a profile name. Names become file names, so the
        ///        accepted set is deliberately narrow: letters, digits,
        ///        underscore and dash, nothing else. That is what keeps a
        ///        name from reaching out of the directory it belongs to.
        /// @param name candidate name
        /// @return true when the name is usable
        [[nodiscard]] static bool ValidName(std::string_view name);

        /// @brief Lists the profiles the directory holds, by name.
        /// @return names, sorted, empty when the directory does not exist
        [[nodiscard]] std::vector<std::string> list() const;

        /// @brief Reads one profile.
        /// @param name profile name
        /// @param[out] valuesOut receives the values, untouched on failure
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the profile was read
        [[nodiscard]] bool load(std::string_view name,
                                TuningValues &valuesOut,
                                std::string &errorOut) const;

        /// @brief Writes one profile, creating the directory if needed and
        ///        replacing a profile of the same name.
        /// @param name profile name
        /// @param values values to store
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the profile was written
        bool save(std::string_view name, const TuningValues &values, std::string &errorOut);

        /// @return directory the profiles live in
        [[nodiscard]] const std::string &directory() const
        {
            return m_directory;
        }

      private:
        /// @param name profile name, already validated
        /// @return path of the file holding it
        [[nodiscard]] std::string pathOf(std::string_view name) const;

        std::string m_directory; ///< directory the profile files live in
    };
} // namespace mark4
