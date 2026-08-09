#include <array>
#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "platform_common/telemetry_packer.hpp"
#include "protocol/header.hpp"
#include "protocol/telemetry.hpp"

TEST_CASE("packTelemetry carries the estimated attitude next to the raw frame")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    frame.gyroRadS = {0.1f, 0.2f, 0.3f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = 101325.0f;
    frame.rc.killSwitch = false;
    frame.rc.armSwitch = true;
    frame.rc.mode = mark4::PilotMode::MANUAL;
    frame.rc.throttle = 0.0f;
    mark4::ActuatorFrame actuators;
    // Enough resting frames to capture the baro reference, then one step in
    // direct-thrust flight (the gyro stays under the resting gate).
    for (std::uint32_t i = 0U; i <= mark4::VerticalEstimator::REFERENCE_SAMPLES; ++i)
    {
        frame.timestampUs = 123456U + static_cast<std::uint64_t>(i) * 2000U;
        core.step(frame, actuators);
    }

    constexpr std::uint16_t TEST_SEQUENCE = 0xA1B2U;
    const auto wire =
        mark4::packTelemetry(frame, actuators, core, mark4::StreamSource::DRONE_SIM, TEST_SEQUENCE);
    REQUIRE(wire.size() == mark4::TELEMETRY_PACKET_SIZE);
    REQUIRE(wire[0] == mark4::PROTOCOL_VERSION);
    REQUIRE(wire[1] == static_cast<std::uint8_t>(mark4::PacketType::TELEMETRY));
    REQUIRE(wire[2] == static_cast<std::uint8_t>(mark4::StreamSource::DRONE_SIM));
    std::uint16_t sequence = 0U;
    std::memcpy(
        &sequence, wire.data() + offsetof(mark4::TelemetryPacket, sequence), sizeof(sequence));
    REQUIRE(sequence == TEST_SEQUENCE);

    std::array<float, 4> quat{};
    std::memcpy(
        quat.data(), wire.data() + offsetof(mark4::TelemetryPacket, attitudeQuat), sizeof(quat));
    const mark4::Quaternion &attitude = core.attitude();
    REQUIRE(quat[0] == attitude.w);
    REQUIRE(quat[1] == attitude.x);
    REQUIRE(quat[2] == attitude.y);
    REQUIRE(quat[3] == attitude.z);

    std::array<float, 4> motor{};
    std::memcpy(motor.data(), wire.data() + offsetof(mark4::TelemetryPacket, motor), sizeof(motor));
    REQUIRE(motor == actuators.motor);

    // Armed, manual, stick down: the core is in direct-thrust flight, and
    // this one byte is what tells a ground station which mode is flying.
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(static_cast<std::uint8_t>(mark4::FlightPhase::MANUAL) == 7U);
    REQUIRE(wire[offsetof(mark4::TelemetryPacket, flightPhase)] ==
            static_cast<std::uint8_t>(mark4::FlightPhase::MANUAL));
}
