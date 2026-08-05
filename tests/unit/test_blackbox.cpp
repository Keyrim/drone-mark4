#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/blackbox.hpp"
#include "flight_core/types.hpp"
#include "platform/log_sink.hpp"

namespace
{
    constexpr std::uint64_t TEST_TIMESTAMP_US = 987654321U;
    constexpr std::array<float, 3> TEST_GYRO_RAD_S = {0.25f, -0.5f, 1.5f};
    constexpr std::array<float, 3> TEST_ACCEL_MPS2 = {0.0f, 0.0f, 9.80665f};
    constexpr float TEST_BARO_PA = 101325.0f;
    constexpr float TEST_THROTTLE = 0.75f;
    constexpr std::array<float, 4> TEST_MOTOR = {0.1f, 0.2f, 0.3f, 0.4f};

    /// Log sink test double: keeps every byte handed to it, and the size of
    /// each individual write, so a test can check both content and framing.
    class LogSinkSpy final : public mark4::AbsLogSink
    {
      public:
        void write(const std::uint8_t *data, std::size_t size) override
        {
            m_writeSizes.push_back(size);
            m_bytes.insert(m_bytes.end(), data, data + size);
        }

        [[nodiscard]] const std::vector<std::uint8_t> &bytes() const
        {
            return m_bytes;
        }

        [[nodiscard]] const std::vector<std::size_t> &writeSizes() const
        {
            return m_writeSizes;
        }

      private:
        std::vector<std::uint8_t> m_bytes;     ///< every byte written, in order
        std::vector<std::size_t> m_writeSizes; ///< size of each write() call
    };

    /// @return a sensor frame with all fields distinctive
    mark4::SensorFrame makeSensorFrame()
    {
        mark4::SensorFrame frame;
        frame.timestampUs = TEST_TIMESTAMP_US;
        frame.gyroRadS = TEST_GYRO_RAD_S;
        frame.accelMps2 = TEST_ACCEL_MPS2;
        frame.baroPa = TEST_BARO_PA;
        frame.rc.killSwitch = false;
        frame.rc.throttle = TEST_THROTTLE;
        return frame;
    }
} // namespace

TEST_CASE("the blackbox record layout is packed and starts with the version byte")
{
    STATIC_REQUIRE(sizeof(mark4::BlackboxRecord) == mark4::BLACKBOX_RECORD_SIZE);
    STATIC_REQUIRE(mark4::BLACKBOX_RECORD_SIZE == 58U);
    STATIC_REQUIRE(std::is_trivially_copyable_v<mark4::BlackboxRecord>);

    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, recordVersion) == 0U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, timestampUs) == 1U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, gyroRadS) == 9U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, accelMps2) == 21U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, baroPa) == 33U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, killSwitch) == 37U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, throttle) == 38U);
    STATIC_REQUIRE(offsetof(mark4::BlackboxRecord, motor) == 42U);
}

TEST_CASE("recording one step writes exactly one record carrying both frames")
{
    LogSinkSpy sink;
    mark4::Blackbox blackbox(sink);

    const mark4::SensorFrame frame = makeSensorFrame();
    mark4::ActuatorFrame actuators;
    actuators.motor = TEST_MOTOR;

    blackbox.record(frame, actuators);

    REQUIRE(sink.writeSizes().size() == 1U);
    REQUIRE(sink.writeSizes()[0] == mark4::BLACKBOX_RECORD_SIZE);
    REQUIRE(sink.bytes().size() == mark4::BLACKBOX_RECORD_SIZE);
    REQUIRE(sink.bytes()[0] == mark4::BLACKBOX_VERSION);

    mark4::BlackboxRecord decoded{};
    std::memcpy(&decoded, sink.bytes().data(), sizeof(decoded));
    REQUIRE(decoded.recordVersion == mark4::BLACKBOX_VERSION);
    REQUIRE(decoded.timestampUs == TEST_TIMESTAMP_US);
    REQUIRE(decoded.baroPa == TEST_BARO_PA);
    REQUIRE(decoded.killSwitch == 0U);
    REQUIRE(decoded.throttle == TEST_THROTTLE);

    /* The array fields are misaligned inside the packed struct: they are read
       straight out of the record bytes instead. */
    std::array<float, 3> gyro{};
    std::array<float, 3> accel{};
    std::array<float, 4> motor{};
    std::memcpy(
        gyro.data(), sink.bytes().data() + offsetof(mark4::BlackboxRecord, gyroRadS), sizeof(gyro));
    std::memcpy(accel.data(),
                sink.bytes().data() + offsetof(mark4::BlackboxRecord, accelMps2),
                sizeof(accel));
    std::memcpy(
        motor.data(), sink.bytes().data() + offsetof(mark4::BlackboxRecord, motor), sizeof(motor));
    REQUIRE(gyro == TEST_GYRO_RAD_S);
    REQUIRE(accel == TEST_ACCEL_MPS2);
    REQUIRE(motor == TEST_MOTOR);
}

TEST_CASE("an engaged kill switch is recorded as one")
{
    LogSinkSpy sink;
    mark4::Blackbox blackbox(sink);

    mark4::SensorFrame frame = makeSensorFrame();
    frame.rc.killSwitch = true;
    const mark4::ActuatorFrame actuators;

    blackbox.record(frame, actuators);

    REQUIRE(sink.bytes()[offsetof(mark4::BlackboxRecord, killSwitch)] == 1U);
}

TEST_CASE("the record count follows the number of recorded steps")
{
    LogSinkSpy sink;
    mark4::Blackbox blackbox(sink);

    const mark4::SensorFrame frame = makeSensorFrame();
    const mark4::ActuatorFrame actuators;

    REQUIRE(blackbox.recordCount() == 0U);
    blackbox.record(frame, actuators);
    blackbox.record(frame, actuators);
    blackbox.record(frame, actuators);
    REQUIRE(blackbox.recordCount() == 3U);
    REQUIRE(sink.bytes().size() == 3U * mark4::BLACKBOX_RECORD_SIZE);
}
