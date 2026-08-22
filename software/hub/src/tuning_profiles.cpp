/// @file
/// @brief Profile storage implementation. Every failure is a returned reason,
///        never an exception escaping to the poll loop: a hand-written file
///        with a typo in it must cost one refused request, not the hub.

#include "hub/tuning_profiles.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace mark4
{
    namespace
    {
        /// Suffix every profile file carries.
        constexpr const char *PROFILE_SUFFIX = ".json";

        /// @param name candidate character
        /// @return true when the character may appear in a profile name
        bool nameCharacter(char character)
        {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '_' || character == '-';
        }

        /// Base of the parameter id keys: JSON object keys are strings, and a
        /// decimal one reads the same in every language that opens the file.
        constexpr unsigned long ID_BASE = 10U;

        /// Digits of the largest parameter id, so a key long enough to
        /// overflow the accumulator below is refused before it is read.
        constexpr std::size_t ID_MAX_DIGITS = 5U;

        /// Largest parameter id: the wire carries it in a u16.
        constexpr unsigned long ID_MAX = 65535U;

        /// @brief Parses a decimal parameter id used as a JSON object key.
        /// @param key key to parse
        /// @param[out] valueOut receives the id
        /// @return true when the key is a decimal number that fits a u16
        bool parseId(const std::string &key, std::uint16_t &valueOut)
        {
            if (key.empty() || key.size() > ID_MAX_DIGITS)
            {
                return false;
            }
            unsigned long parsed = 0U;
            for (const char character : key)
            {
                if (character < '0' || character > '9')
                {
                    return false;
                }
                parsed = parsed * ID_BASE + static_cast<unsigned long>(character - '0');
            }
            if (parsed > ID_MAX)
            {
                return false;
            }
            valueOut = static_cast<std::uint16_t>(parsed);
            return true;
        }
    } // namespace

    TuningProfiles::TuningProfiles(std::string directory)
        : m_directory(std::move(directory))
    {
    }

    bool TuningProfiles::ValidName(std::string_view name)
    {
        if (name.empty() || name.size() > MAX_NAME_LENGTH)
        {
            return false;
        }
        return std::all_of(name.begin(), name.end(), nameCharacter);
    }

    std::string TuningProfiles::pathOf(std::string_view name) const
    {
        return m_directory + "/" + std::string(name) + PROFILE_SUFFIX;
    }

    std::vector<std::string> TuningProfiles::list() const
    {
        std::vector<std::string> names;
        std::error_code code;
        // The non-throwing overloads throughout: a directory that is not
        // there yet is an empty list, not a failure.
        for (const auto &entry : std::filesystem::directory_iterator(m_directory, code))
        {
            const std::string fileName = entry.path().filename().string();
            const std::string suffix = PROFILE_SUFFIX;
            if (fileName.size() <= suffix.size() ||
                fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0)
            {
                continue;
            }
            const std::string name = fileName.substr(0U, fileName.size() - suffix.size());
            if (ValidName(name))
            {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool TuningProfiles::load(std::string_view name,
                              TuningValues &valuesOut,
                              std::string &errorOut) const
    {
        if (!ValidName(name))
        {
            errorOut = "invalid profile name";
            return false;
        }
        const std::string path = pathOf(name);
        std::ifstream file(path);
        if (!file.is_open())
        {
            errorOut = "no profile '" + std::string(name) + "'";
            return false;
        }

        const auto root = nlohmann::json::parse(file, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            errorOut = "profile '" + std::string(name) + "' is not a JSON object";
            return false;
        }
        const auto version = root.find("version");
        if (version == root.end() || !version->is_number_integer() ||
            version->get<int>() != FORMAT_VERSION)
        {
            errorOut = "profile '" + std::string(name) + "' has an unsupported version";
            return false;
        }
        const auto values = root.find("values");
        if (values == root.end() || !values->is_object())
        {
            errorOut = "profile '" + std::string(name) + "' carries no values object";
            return false;
        }

        TuningValues parsed;
        for (const auto &entry : values->items())
        {
            std::uint16_t id = 0U;
            if (!parseId(entry.key(), id) || !entry.value().is_number())
            {
                errorOut = "profile '" + std::string(name) + "' has an entry that is not id:number";
                return false;
            }
            parsed[id] = static_cast<float>(entry.value().get<double>());
        }
        valuesOut = std::move(parsed);
        return true;
    }

    bool TuningProfiles::save(std::string_view name,
                              const TuningValues &values,
                              std::string &errorOut)
    {
        if (!ValidName(name))
        {
            errorOut = "invalid profile name";
            return false;
        }
        std::error_code code;
        static_cast<void>(std::filesystem::create_directories(m_directory, code));

        nlohmann::ordered_json valuesObject = nlohmann::ordered_json::object();
        for (const auto &[id, value] : values)
        {
            // Ids are decimal strings: JSON object keys are strings, and a
            // decimal one reads the same in every language that opens this.
            valuesObject[std::to_string(id)] = static_cast<double>(value);
        }
        nlohmann::ordered_json root;
        root["version"] = FORMAT_VERSION;
        root["values"] = valuesObject;

        const std::string path = pathOf(name);
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open())
        {
            errorOut = "cannot write " + path;
            return false;
        }
        file << root.dump(2) << "\n";
        if (!file.good())
        {
            errorOut = "cannot write " + path;
            return false;
        }
        return true;
    }
} // namespace mark4
