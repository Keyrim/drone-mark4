#pragma once

/// @file
/// @brief Static registry of the runtime tunable parameters.

#include <array>
#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Outcome of a tuning operation. OK is 0 so a zero-initialized status is
    /// a valid success. The values and their order mirror the wire-level
    /// TUNING_ACK_* statuses one for one: the platform adapter that will
    /// bridge the two maps one onto the other, and flight-core keeps its own
    /// copy so it never includes a wire header.
    enum class TuningStatus : std::uint8_t
    {
        OK = 0U,                 ///< value accepted and applied
        UNKNOWN_ID = 1U,         ///< no parameter carries this id
        OUT_OF_BOUNDS = 2U,      ///< value outside [minValue, maxValue], NaN included
        LOCKED_WHILE_ARMED = 3U, ///< parameter refuses changes while armed
    };

    /// One entry of the tuning registry: the live value plus everything a
    /// ground station needs to display and validate it without a hardcoded
    /// table of its own.
    struct TuningParam
    {
        /// Size of the name field, matching the wire-level TUNING_NAME_SIZE.
        /// Deliberately its own constant rather than that one: flight-core
        /// never includes a protocol header. The adapter that bridges the two
        /// is where the two constants meet, and where they must be asserted
        /// equal.
        static constexpr std::size_t NAME_SIZE = 16U;

        std::uint16_t id;                 ///< stable identifier, never reused
        std::array<char, NAME_SIZE> name; ///< ASCII, zero padded, unterminated when full
        float value;                      ///< live value, the one the owning module uses
        float minValue;                   ///< smallest accepted value, inclusive
        float maxValue;                   ///< largest accepted value, inclusive
        bool armedChange;                 ///< true when the value may change while armed
    };

    // Parameter ids, grouped by owning module. Append only: an id is never
    // reused nor renumbered, and a parameter that disappears retires its id
    // forever, so a recorded log or an old ground station can never read a
    // value as something else. 0 is never a valid id. The 6xx range is
    // reserved for the flight core's own safety thresholds.

    /// Rate controller, 1xx.
    inline constexpr std::uint16_t TUNING_ID_RATE_KP_ROLL_PITCH = 101U;
    inline constexpr std::uint16_t TUNING_ID_RATE_KI_ROLL_PITCH = 102U;
    inline constexpr std::uint16_t TUNING_ID_RATE_KP_YAW = 103U;
    inline constexpr std::uint16_t TUNING_ID_RATE_KI_YAW = 104U;

    /// Attitude controller, 2xx.
    inline constexpr std::uint16_t TUNING_ID_ATTITUDE_KP = 201U;

    /// Vertical controller, 3xx.
    inline constexpr std::uint16_t TUNING_ID_VERTICAL_KP = 301U;
    inline constexpr std::uint16_t TUNING_ID_VERTICAL_KI = 302U;
    inline constexpr std::uint16_t TUNING_ID_HOVER_COLLECTIVE = 303U;

    /// Attitude estimator, 4xx.
    inline constexpr std::uint16_t TUNING_ID_AHRS_KP = 401U;
    inline constexpr std::uint16_t TUNING_ID_AHRS_KI = 402U;

    /// Vertical estimator, 5xx.
    inline constexpr std::uint16_t TUNING_ID_VEST_ALTITUDE_GAIN = 501U;
    inline constexpr std::uint16_t TUNING_ID_VEST_VELOCITY_GAIN = 502U;

    /// Stick mapping, 7xx: the feel of the piloted modes.
    inline constexpr std::uint16_t TUNING_ID_STICK_RATE_ROLL_PITCH = 701U;
    inline constexpr std::uint16_t TUNING_ID_STICK_RATE_YAW = 702U;
    inline constexpr std::uint16_t TUNING_ID_STICK_TILT_MAX = 703U;
    inline constexpr std::uint16_t TUNING_ID_STICK_DEADBAND = 704U;

    /// Fixed-size registry of the tunable parameters: their live values, their
    /// bounds and whether they may move while the drone is armed. No
    /// allocation, no ordering assumption, lookup by id.
    ///
    /// The table validates and stores; it does not reach into the modules that
    /// own the values. Whoever owns both (the flight core) applies an accepted
    /// value to its module in the same call, so the table and the modules
    /// never disagree.
    class TuningTable
    {
      public:
        /// Number of registered parameters.
        static constexpr std::size_t PARAM_COUNT = 16U;

        /// @brief Builds the registry with every value at its module's default.
        TuningTable();

        /// @brief Validates a new value and stores it on success. The checks
        ///        run in this order: unknown id, then the armed lock, then the
        ///        bounds - so a caller learns about a wrong id before anything
        ///        else, and a locked parameter never leaks its bounds.
        /// @param id parameter identifier
        /// @param value candidate value
        /// @param armed true when the drone is armed or flying
        /// @return OK when the value was stored, the reason otherwise
        TuningStatus set(std::uint16_t id, float value, bool armed);

        /// @brief Reads the live value of a parameter.
        /// @param id parameter identifier
        /// @param[out] valueOut live value, written only when the call returns OK
        /// @return OK, or UNKNOWN_ID when no parameter carries this id
        [[nodiscard]] TuningStatus get(std::uint16_t id, float &valueOut) const;

        /// @brief Enumerates the registry, for a ground station discovering it.
        /// @param index position in the registry, in [0, PARAM_COUNT)
        /// @return the entry, or nullptr past the end
        [[nodiscard]] const TuningParam *info(std::size_t index) const;

      private:
        [[nodiscard]] TuningParam *find(std::uint16_t id);
        [[nodiscard]] const TuningParam *find(std::uint16_t id) const;

        std::array<TuningParam, PARAM_COUNT> m_params; ///< the registry itself
    };
} // namespace mark4
