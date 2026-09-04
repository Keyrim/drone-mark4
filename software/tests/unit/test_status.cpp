/// @file
/// @brief The Status report: what the fixed periodic snapshot carries and
///        what it deliberately no longer carries (measurements are
///        telemetry measures now).

#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "platform_common/status_packer.hpp"
#include "protocol/envelope.hpp"

TEST_CASE("packStatus carries the estimated attitude, the motors and the phase")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    frame.gyroRadS = {0.1f, 0.2f, 0.3f};
    frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
    frame.baroPa = 101325.0f;
    frame.imuValid = true;
    frame.baroValid = true;
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

    mark4_Status status;
    mark4::packStatus(frame, actuators, core, true, status);
    REQUIRE(status.timestamp_us == frame.timestampUs);

    const mark4::Quaternion &attitude = core.attitude();
    REQUIRE(status.attitude_quat[0] == attitude.w);
    REQUIRE(status.attitude_quat[1] == attitude.x);
    REQUIRE(status.attitude_quat[2] == attitude.y);
    REQUIRE(status.attitude_quat[3] == attitude.z);
    for (std::size_t motor = 0U; motor < actuators.motor.size(); ++motor)
    {
        REQUIRE(status.motor[motor] == actuators.motor[motor]);
    }
    // No plant behind a packed frame: the truth is a sim composition's to add.
    REQUIRE(!status.has_truth);

    // Armed, manual, stick down: the core is in direct-thrust flight, and
    // this one field is what tells a ground station which mode is flying.
    REQUIRE(core.flightPhase() == mark4::FlightPhase::MANUAL);
    REQUIRE(status.flight_phase == mark4_FlightPhase_PHASE_MANUAL);
    REQUIRE(static_cast<int>(status.flight_phase) == 7);

    // The validity flags travel as the platform set them on this frame.
    REQUIRE(status.imu_valid);
    REQUIRE(status.baro_valid);
    frame.baroValid = false;
    mark4::packStatus(frame, actuators, core, true, status);
    REQUIRE(status.imu_valid);
    REQUIRE(!status.baro_valid);
    frame.baroValid = true;

    // The RC link flag is the composition's word, not the frame's: a frame
    // carrying the kill looks the same whether a pilot holds it or the
    // fail-safe grafted it, so the packer is told.
    mark4::packStatus(frame, actuators, core, false, status);
    REQUIRE(!status.rc_link_ok);
    mark4::packStatus(frame, actuators, core, true, status);
    REQUIRE(status.rc_link_ok);

    // And the message survives the wire.
    mark4_Envelope envelope = mark4_Envelope_init_zero;
    envelope.which_body = mark4_Envelope_status_tag;
    envelope.body.status = status;
    std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
    std::size_t size = 0U;
    REQUIRE(mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size));
    mark4_Envelope decoded;
    REQUIRE(mark4::decodeEnvelope(bytes.data(), size, decoded));
    REQUIRE(decoded.which_body == mark4_Envelope_status_tag);
    REQUIRE(decoded.body.status.attitude_quat[0] == attitude.w);
    REQUIRE(decoded.body.status.flight_phase == mark4_FlightPhase_PHASE_MANUAL);
}

TEST_CASE("the throw state and count are the only throw facts Status carries")
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    frame.imuValid = true;
    mark4::ActuatorFrame actuators;
    frame.timestampUs = 1000U;
    core.step(frame, actuators);

    mark4_Status status;
    mark4::packStatus(frame, actuators, core, false, status);
    REQUIRE(status.throw_state == mark4_ThrowState_THROW_IDLE);
    REQUIRE(status.throw_count == 0U);
    // The release velocity, the apex and its instant left the wire as
    // fields: they are telemetry measures, read where they are computed.
}
