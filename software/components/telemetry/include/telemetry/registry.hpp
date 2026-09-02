#pragma once

/// @file
/// @brief The telemetry registry: named float measures declared next to the
///        variables they read, and the process-wide list a wire adapter
///        walks to answer a ground tool.
///
/// Usage, next to the value being measured:
/// @code
///     class AttitudeEstimator
///     {
///         ...
///       private:
///         float m_rollRad = 0.0f;
///         TelemetryEntry m_rollEntry{"estimator/attitude/roll",
///                                    TelemetryUnit::RAD,
///                                    m_rollRad};
///     };
/// @endcode
/// An entry keeps a pointer to the value, so the value must outlive it (a
/// member of the same object is the normal case). It knows names, units and
/// where values live; it knows nothing of the wire, of ids or of periods -
/// those belong to the adapter that freezes the list into a table
/// (`platform_common/telemetry_service.hpp`).
///
/// Not thread-safe: every node of the project registers and samples from
/// its one loop thread.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Physical dimension of one measure. Same values as the wire's
    /// mark4.TelemetryUnit, which the wire adapter asserts one by one: a
    /// leaf library cannot include a generated protobuf header.
    enum class TelemetryUnit : std::uint8_t
    {
        UNITLESS = 0U, ///< ratios, quaternion components, enum codes, booleans as 0/1
        M = 1U,
        M_PER_S = 2U,
        M_PER_S2 = 3U,
        RAD = 4U,
        RAD_PER_S = 5U,
        PA = 6U,
        CELSIUS = 7U,
        V = 8U,
        A = 9U,
        US = 10U,
        COUNT = 11U, ///< counters
    };

    /// Longest measure name, terminator excluded (mark4.TelemetryDescriptor).
    inline constexpr std::size_t MAX_TELEMETRY_NAME = 40U;

    /// Measures one wire adapter indexes at once. The registry itself has no
    /// limit; this is the size of the frozen table an adapter builds, and
    /// therefore the largest id that can travel.
    inline constexpr std::size_t MAX_TELEMETRY_ENTRIES = 128U;

    /// One measure the node exposes: a name, a unit and where to read the
    /// value. Meant as a member of the object that owns the value, declared
    /// after it; the constructor links it into the process-wide registry in
    /// construction order and the destructor unlinks it, so objects that
    /// come and go (a flight core rebuilt on a reset, several of them in a
    /// test) leave no dangling entry behind.
    class TelemetryEntry
    {
      public:
        /// Reads a quantity that is not a plain float member.
        using ReadFn = float (*)(const void *context);

        /// @brief Registers a measure read straight out of a float.
        /// @param name hierarchical name, "area/thing", at most
        ///        MAX_TELEMETRY_NAME characters, kept by pointer (a literal)
        /// @param unit physical dimension of the value
        /// @param value the measured variable; must outlive this entry
        TelemetryEntry(const char *name, TelemetryUnit unit, const float &value);

        /// @brief Registers a measure computed on read. For enums, booleans
        ///        and quantities derived from several fields; prefer keeping
        ///        a float member up to date once per step.
        /// @param name hierarchical name, "area/thing", at most
        ///        MAX_TELEMETRY_NAME characters, kept by pointer (a literal)
        /// @param unit physical dimension of the value
        /// @param context handed back to read, unchanged; must outlive this
        ///        entry
        /// @param read reader, never nullptr
        TelemetryEntry(const char *name, TelemetryUnit unit, const void *context, ReadFn read);

        TelemetryEntry(const TelemetryEntry &) = delete;
        TelemetryEntry &operator=(const TelemetryEntry &) = delete;
        TelemetryEntry(TelemetryEntry &&) = delete;
        TelemetryEntry &operator=(TelemetryEntry &&) = delete;

        /// @brief Unlinks the entry from the registry.
        ~TelemetryEntry();

        /// @return the measure's current value
        [[nodiscard]] float read() const
        {
            return m_read != nullptr ? m_read(m_context) : *m_value;
        }

        [[nodiscard]] const char *name() const
        {
            return m_name;
        }

        [[nodiscard]] TelemetryUnit unit() const
        {
            return m_unit;
        }

        /// @return next entry of the registry, nullptr at the end
        [[nodiscard]] TelemetryEntry *next() const
        {
            return m_next;
        }

      private:
        /// @brief Appends this entry to the registry, keeping the list in
        ///        construction order.
        void link();

        const char *m_name;               ///< hierarchical name
        TelemetryUnit m_unit;             ///< physical dimension
        const float *m_value = nullptr;   ///< measured variable, pointer form
        const void *m_context = nullptr;  ///< handed to m_read
        ReadFn m_read = nullptr;          ///< reader, nullptr for the pointer form
        TelemetryEntry *m_next = nullptr; ///< registry link
    };

    /// @return first entry of the registry in construction order, nullptr
    ///         when none is declared
    TelemetryEntry *telemetryEntries();

    /// @return registered entries
    std::size_t telemetryEntryCount();
} // namespace mark4
