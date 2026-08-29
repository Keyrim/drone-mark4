#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "platform_common/telemetry_packer.hpp"
#include "protocol/envelope.hpp"

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

    mark4_Telemetry telemetry;
    mark4::packTelemetry(frame, actuators, core, telemetry);
    REQUIRE(telemetry.timestamp_us == frame.timestampUs);
    REQUIRE(telemetry.gyro_rad_s[0] == 0.1f);
    REQUIRE(telemetry.gyro_rad_s[1] == 0.2f);
    REQUIRE(telemetry.gyro_rad_s[2] == 0.3f);

    const mark4::Quaternion &attitude = core.attitude();
    REQUIRE(telemetry.attitude_quat[0] == attitude.w);
    REQUIRE(telemetry.attitude_quat[1] == attitude.x);
    REQUIRE(telemetry.attitude_quat[2] == attitude.y);
    REQUIRE(telemetry.attitude_quat[3] == attitude.z);
    for (std::size_t motor = 0U; motor < actuators.motor.size(); ++motor)
    {
        REQUIRE(telemetry.motor[motor] == actuators.motor[motor]);
    }
    REQUIRE(telemetry.baro_altitude_m == core.baroAltitudeM());
    // No plant behind a packed frame: the truth is a sim composition's to add.
    REQUIRE(!telemetry.has_truth);

    // Armed, manual, stick down: the core is in direct-thrust flight, and
    // this one field is what tells a ground station which mode is flying.
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(telemetry.flight_phase == mark4_FlightPhase_PHASE_MANUAL);
    REQUIRE(static_cast<int>(telemetry.flight_phase) == 7);

    // And the message survives the wire.
    mark4_Envelope envelope = mark4_Envelope_init_zero;
    envelope.which_body = mark4_Envelope_telemetry_tag;
    envelope.body.telemetry = telemetry;
    std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
    std::size_t size = 0U;
    REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
    mark4_Envelope decoded;
    REQUIRE(mark4::decodeEnvelope(bytes.data(), size, decoded));
    REQUIRE(decoded.which_body == mark4_Envelope_telemetry_tag);
    REQUIRE(decoded.body.telemetry.attitude_quat[0] == attitude.w);
    REQUIRE(decoded.body.telemetry.flight_phase == mark4_FlightPhase_PHASE_MANUAL);
}
