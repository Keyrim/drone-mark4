/// @file
/// @brief Profile storage: what round trips, what is refused, and what a
///        hand-edited file that went wrong costs.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

#include "hub/tuning_profiles.hpp"

namespace
{
    /// A directory of its own per test, removed on the way out. Profiles are
    /// files, so exercising them means touching a real filesystem.
    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            std::error_code code;
            m_path = std::filesystem::temp_directory_path(code) /
                     ("mark4_profiles_" + std::to_string(::getpid()) + "_" +
                      std::to_string(s_counter++));
            static_cast<void>(std::filesystem::remove_all(m_path, code));
        }

        ScratchDirectory(const ScratchDirectory &) = delete;
        ScratchDirectory &operator=(const ScratchDirectory &) = delete;
        ScratchDirectory(ScratchDirectory &&) = delete;
        ScratchDirectory &operator=(ScratchDirectory &&) = delete;

        ~ScratchDirectory()
        {
            std::error_code code;
            static_cast<void>(std::filesystem::remove_all(m_path, code));
        }

        /// @return the directory path, as the profiles store wants it
        [[nodiscard]] std::string path() const
        {
            return m_path.string();
        }

        /// @brief Writes a file into the directory, creating it if needed.
        /// @param fileName name of the file
        /// @param content bytes to write
        void write(const std::string &fileName, const std::string &content) const
        {
            std::error_code code;
            static_cast<void>(std::filesystem::create_directories(m_path, code));
            std::ofstream file(m_path / fileName, std::ios::trunc);
            file << content;
        }

      private:
        std::filesystem::path m_path;     ///< directory owned by this test
        static inline unsigned s_counter; ///< keeps two directories apart
    };
} // namespace

TEST_CASE("a profile round trips through the directory")
{
    const ScratchDirectory scratch;
    mark4::TuningProfiles profiles(scratch.path());
    REQUIRE(profiles.list().empty()); // a directory that is not there yet

    mark4::TuningValues values;
    values[101U] = 0.028f;
    values[303U] = 0.55f;
    std::string error;
    REQUIRE(profiles.save("bench", values, error));
    REQUIRE(error.empty());

    mark4::TuningValues read;
    REQUIRE(profiles.load("bench", read, error));
    REQUIRE(read.size() == 2U);
    REQUIRE(read.at(101U) == 0.028f);
    REQUIRE(read.at(303U) == 0.55f);

    // Saving again under the same name replaces, it does not accumulate.
    values[101U] = 0.030f;
    REQUIRE(profiles.save("bench", values, error));
    REQUIRE(profiles.load("bench", read, error));
    REQUIRE(read.at(101U) == 0.030f);

    REQUIRE(profiles.save("field-2", values, error));
    const std::vector<std::string> names = profiles.list();
    REQUIRE(names.size() == 2U);
    REQUIRE(names[0] == "bench");
    REQUIRE(names[1] == "field-2");
}

TEST_CASE("an unknown profile is refused with a reason, not a crash")
{
    const ScratchDirectory scratch;
    const mark4::TuningProfiles profiles(scratch.path());

    mark4::TuningValues values;
    values[1U] = 1.0f;
    mark4::TuningValues read = values;
    std::string error;
    REQUIRE(!profiles.load("nothing", read, error));
    REQUIRE(!error.empty());
    // The output is untouched by a failed read.
    REQUIRE(read == values);
}

TEST_CASE("a malformed profile file is refused and the rest still lists")
{
    const ScratchDirectory scratch;
    mark4::TuningProfiles profiles(scratch.path());

    scratch.write("broken.json", "{not json at all");
    scratch.write("array.json", "[1, 2, 3]");
    scratch.write("wrongversion.json", R"({"version":99,"values":{}})");
    scratch.write("novalues.json", R"({"version":1})");
    scratch.write("badkey.json", R"({"version":1,"values":{"kp":0.1}})");
    scratch.write("badvalue.json", R"({"version":1,"values":{"101":"fast"}})");
    scratch.write("notaprofile.txt", "ignored");

    mark4::TuningValues values;
    std::string error;
    for (const char *name : {"broken", "array", "wrongversion", "novalues", "badkey", "badvalue"})
    {
        INFO("profile: " << name);
        REQUIRE(!profiles.load(name, values, error));
        REQUIRE(!error.empty());
    }

    // A directory full of rubbish still lists its .json files, and a good
    // one saved next to them reads back fine: one bad file costs itself.
    values[101U] = 0.1f;
    REQUIRE(profiles.save("good", values, error));
    mark4::TuningValues read;
    REQUIRE(profiles.load("good", read, error));
    REQUIRE(read.at(101U) == 0.1f);

    const std::vector<std::string> names = profiles.list();
    REQUIRE(names.size() == 7U);
    REQUIRE(std::find(names.begin(), names.end(), "good") != names.end());
    // Only .json files are profiles.
    REQUIRE(std::find(names.begin(), names.end(), "notaprofile") == names.end());
}

TEST_CASE("a profile name is checked before it can become a file name")
{
    REQUIRE(mark4::TuningProfiles::ValidName("bench"));
    REQUIRE(mark4::TuningProfiles::ValidName("Field_2-b"));
    REQUIRE(mark4::TuningProfiles::ValidName("x"));
    REQUIRE(mark4::TuningProfiles::ValidName(std::string(32U, 'a')));

    REQUIRE(!mark4::TuningProfiles::ValidName(""));
    REQUIRE(!mark4::TuningProfiles::ValidName(std::string(33U, 'a')));
    // Anything that could reach out of the directory, or confuse a shell.
    REQUIRE(!mark4::TuningProfiles::ValidName(".."));
    REQUIRE(!mark4::TuningProfiles::ValidName("../etc/passwd"));
    REQUIRE(!mark4::TuningProfiles::ValidName("/absolute"));
    REQUIRE(!mark4::TuningProfiles::ValidName("with space"));
    REQUIRE(!mark4::TuningProfiles::ValidName("dot.json"));

    // And the store refuses them too, rather than trusting its caller.
    const ScratchDirectory scratch;
    mark4::TuningProfiles profiles(scratch.path());
    mark4::TuningValues values;
    values[101U] = 0.1f;
    std::string error;
    REQUIRE(!profiles.save("../escape", values, error));
    REQUIRE(!error.empty());
    mark4::TuningValues read;
    REQUIRE(!profiles.load("../escape", read, error));
}
