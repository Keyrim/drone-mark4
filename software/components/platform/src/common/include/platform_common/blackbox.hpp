#pragma once

/// @file
/// @brief Blackbox recorder: serializes flight data into protocol/ records
///        and hands them to a log sink. An IO adapter of the composition
///        layer: flight-core never calls it and never sees the wire.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "flight_core/types.hpp"
#include "platform/log_sink.hpp"
#include "protocol/blackbox.hpp"

namespace mark4
{
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
        void record(const SensorFrame &sensors, const ActuatorFrame &actuators)
        {
            BlackboxRecord entry{};
            entry.sync0 = BLACKBOX_SYNC0;
            entry.sync1 = BLACKBOX_SYNC1;
            entry.version = BLACKBOX_VERSION;
            entry.type = static_cast<std::uint8_t>(PacketType::BLACKBOX_RECORD);
            entry.length = BLACKBOX_RECORD_PAYLOAD_SIZE;
            entry.timestampUs = sensors.timestampUs;
            entry.baroPa = sensors.baroPa;
            entry.killSwitch = sensors.rc.killSwitch ? 1U : 0U;
            entry.throttle = sensors.rc.throttle;
            entry.armSwitch = sensors.rc.armSwitch ? 1U : 0U;

            std::array<std::uint8_t, BLACKBOX_RECORD_SIZE> bytes{};
            std::memcpy(bytes.data(), &entry, sizeof(entry));
            /* The std::array members sit at odd offsets in the packed struct:
               assigning them through it would bind a reference to a misaligned
               address, so they are copied straight into the record bytes. */
            std::memcpy(bytes.data() + offsetof(BlackboxRecord, gyroRadS),
                        sensors.gyroRadS.data(),
                        sizeof(sensors.gyroRadS));
            std::memcpy(bytes.data() + offsetof(BlackboxRecord, accelMps2),
                        sensors.accelMps2.data(),
                        sizeof(sensors.accelMps2));
            std::memcpy(bytes.data() + offsetof(BlackboxRecord, motor),
                        actuators.motor.data(),
                        sizeof(actuators.motor));

            const std::uint16_t crc = blackboxRecordCrc(bytes.data());
            bytes[BLACKBOX_CRC_OFFSET] = static_cast<std::uint8_t>(crc & CRC16_LOW_MASK);
            bytes[BLACKBOX_CRC_OFFSET + 1U] = static_cast<std::uint8_t>(crc >> CRC16_BYTE_SHIFT);

            m_sink.write(bytes.data(), bytes.size());
            ++m_recordCount;
        }

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
