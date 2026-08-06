#include "flight_core/blackbox.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mark4
{
    void Blackbox::record(const SensorFrame &sensors, const ActuatorFrame &actuators)
    {
        BlackboxRecord entry{};
        entry.recordVersion = BLACKBOX_VERSION;
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

        m_sink.write(bytes.data(), bytes.size());
        ++m_recordCount;
    }
} // namespace mark4
