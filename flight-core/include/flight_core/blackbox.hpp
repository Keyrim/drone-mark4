#pragma once

/// @file
/// @brief Blackbox record format and recorder.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "flight_core/types.hpp"
#include "platform/log_sink.hpp"

namespace mark4
{
    /// Version byte carried by every blackbox record. It describes the on-disk
    /// log layout only and is versioned on its own: it has nothing to do with
    /// the UDP wire structs in protocol/ and the two evolve independently.
    inline constexpr std::uint8_t BLACKBOX_VERSION = 1U;

#pragma pack(push, 1)
    /// One recorded step: the sensor frame that entered the core and the
    /// actuator frame it produced. Packed and fixed size, so a log file is a
    /// plain sequence of records that any decoder can split by size alone.
    struct BlackboxRecord
    {
        std::uint8_t recordVersion;     ///< = BLACKBOX_VERSION
        std::uint64_t timestampUs;      ///< acquisition time [us]
        std::array<float, 3> gyroRadS;  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa;                   ///< static pressure [Pa]
        std::uint8_t killSwitch;        ///< 1 = engaged (motors cut), 0 = released
        float throttle;                 ///< normalized RC throttle [0, 1]
        std::array<float, 4> motor;     ///< normalized motor commands [0, 1]
    };
#pragma pack(pop)

    /// Packed record size: version (1) + timestamp (8) + gyro (12) + accel (12)
    /// + baro (4) + kill switch (1) + throttle (4) + motors (16).
    inline constexpr std::size_t BLACKBOX_RECORD_SIZE = 58U;

    static_assert(sizeof(BlackboxRecord) == BLACKBOX_RECORD_SIZE, "record layout must be packed");
    static_assert(std::is_trivially_copyable_v<BlackboxRecord>);
    static_assert(
        offsetof(BlackboxRecord, recordVersion) == 0U,
        "the version byte must come first, so a decoder can read it before anything else");

    /// Serializes flight data into blackbox records and hands them to a log
    /// sink. Pure: no allocation, no clock access, no formatting - the sink
    /// alone decides where the bytes end up.
    class Blackbox
    {
      public:
        /// @param sink log sink the records are written to, owned by the caller
        explicit Blackbox(AbsLogSink &sink)
            : m_sink(sink)
        {
        }

        /// @brief Serializes one step and appends it to the sink.
        /// @param sensors sensor frame that entered the core
        /// @param actuators actuator frame the core produced for it
        void record(const SensorFrame &sensors, const ActuatorFrame &actuators);

        /// @return number of records written since construction
        [[nodiscard]] std::uint32_t recordCount() const
        {
            return m_recordCount;
        }

      private:
        AbsLogSink &m_sink;               ///< log sink, owned by the composition root
        std::uint32_t m_recordCount = 0U; ///< records written since construction
    };
} // namespace mark4
