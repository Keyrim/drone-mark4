/// @file
/// @brief The telemetry registry: what linking and unlinking does to the
///        process-wide list, and what the two kinds of entry read.
///
/// The registry is process-wide and every other object built by this binary
/// registers into it, so every check here is relative to the count observed
/// on entry and looks entries up by name.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "telemetry/registry.hpp"

namespace
{
    /// @param name name to look for
    /// @return the entry with that name, nullptr when none has it
    const mark4::TelemetryEntry *find(const char *name)
    {
        for (const mark4::TelemetryEntry *entry = mark4::telemetryEntries(); entry != nullptr;
             entry = entry->next())
        {
            if (std::string(entry->name()) == name)
            {
                return entry;
            }
        }
        return nullptr;
    }

    /// @return the names of every registered entry, in list order
    std::vector<std::string> names()
    {
        std::vector<std::string> all;
        for (const mark4::TelemetryEntry *entry = mark4::telemetryEntries(); entry != nullptr;
             entry = entry->next())
        {
            all.emplace_back(entry->name());
        }
        return all;
    }

    /// @param names list to search
    /// @param name name to locate
    /// @return its index, empty when absent
    std::optional<std::size_t> indexOf(const std::vector<std::string> &names, const char *name)
    {
        for (std::size_t index = 0U; index < names.size(); ++index)
        {
            if (names[index] == name)
            {
                return index;
            }
        }
        return std::nullopt;
    }

    /// A value with a measure next to it, the normal shape of a registration.
    class Measured
    {
      public:
        /// @param name name of the measure
        explicit Measured(const char *name)
            : m_entry(name, mark4::TelemetryUnit::M_PER_S, m_value)
        {
        }

        /// @param value new value of the measure
        void set(float value)
        {
            m_value = value;
        }

      private:
        float m_value = 0.0f;          ///< the measured variable
        mark4::TelemetryEntry m_entry; ///< reads m_value by pointer
    };

    /// A quantity that is not a float: read through the reader form.
    class Counted
    {
      public:
        Counted()
            : m_entry("test/counted", mark4::TelemetryUnit::COUNT, this, &Counted::Read)
        {
        }

        void bump()
        {
            ++m_count;
        }

      private:
        /// @param context the Counted the entry was built with
        /// @return the count as a float
        static float Read(const void *context)
        {
            return static_cast<float>(static_cast<const Counted *>(context)->m_count);
        }

        std::uint64_t m_count = 0U;    ///< what the reader converts
        mark4::TelemetryEntry m_entry; ///< reads m_count through Read
    };
} // namespace

TEST_CASE("a pointer entry reads the live value and unlinks when its owner dies")
{
    const std::size_t before = mark4::telemetryEntryCount();
    REQUIRE(find("test/speed") == nullptr);
    {
        Measured measured("test/speed");
        REQUIRE(mark4::telemetryEntryCount() == before + 1U);
        const mark4::TelemetryEntry *entry = find("test/speed");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->unit() == mark4::TelemetryUnit::M_PER_S);
        REQUIRE(entry->read() == 0.0f);

        // No copy is taken: the entry reads whatever the variable holds now.
        measured.set(-4.5f);
        REQUIRE(entry->read() == -4.5f);
    }
    REQUIRE(mark4::telemetryEntryCount() == before);
    REQUIRE(find("test/speed") == nullptr);
}

TEST_CASE("a reader entry converts a quantity that is not a float")
{
    const std::size_t before = mark4::telemetryEntryCount();
    Counted counted;
    const mark4::TelemetryEntry *entry = find("test/counted");
    REQUIRE(entry != nullptr);
    REQUIRE(entry->unit() == mark4::TelemetryUnit::COUNT);
    REQUIRE(entry->read() == 0.0f);
    counted.bump();
    counted.bump();
    REQUIRE(entry->read() == 2.0f);
    REQUIRE(mark4::telemetryEntryCount() == before + 1U);
}

TEST_CASE("the list stays in construction order")
{
    Measured first("test/order_a");
    Measured second("test/order_b");
    Measured third("test/order_c");

    const std::vector<std::string> all = names();
    const std::optional<std::size_t> a = indexOf(all, "test/order_a");
    const std::optional<std::size_t> b = indexOf(all, "test/order_b");
    const std::optional<std::size_t> c = indexOf(all, "test/order_c");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    // Ids are indexes into this order, so it must be construction order and
    // not the reverse a head-insertion list would give.
    REQUIRE(*a < *b);
    REQUIRE(*b < *c);
    static_cast<void>(first);
    static_cast<void>(second);
    static_cast<void>(third);
}

TEST_CASE("entries unlink in any order, head, middle and tail alike")
{
    const std::size_t before = mark4::telemetryEntryCount();
    {
        auto head = std::make_unique<Measured>("test/unlink_head");
        auto middle = std::make_unique<Measured>("test/unlink_middle");
        auto tail = std::make_unique<Measured>("test/unlink_tail");
        REQUIRE(mark4::telemetryEntryCount() == before + 3U);

        middle.reset();
        REQUIRE(find("test/unlink_middle") == nullptr);
        REQUIRE(find("test/unlink_head") != nullptr);
        REQUIRE(find("test/unlink_tail") != nullptr);
        REQUIRE(mark4::telemetryEntryCount() == before + 2U);

        head.reset();
        REQUIRE(find("test/unlink_head") == nullptr);
        REQUIRE(find("test/unlink_tail") != nullptr);

        // The tail goes last: the list must still accept a new entry after
        // its own tail pointer moved back.
        tail.reset();
        REQUIRE(mark4::telemetryEntryCount() == before);
        Measured again("test/unlink_again");
        REQUIRE(find("test/unlink_again") != nullptr);
        REQUIRE(mark4::telemetryEntryCount() == before + 1U);
    }
    REQUIRE(mark4::telemetryEntryCount() == before);
}
