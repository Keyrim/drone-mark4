#pragma once

/// @file
/// @brief Conversion of the flight core state into the Telemetry message.
///        An IO adapter of the composition layer: it reads only public
///        accessors, so flight-core never sees a wire type. This is also
///        where the flight core enums and their wire counterparts are pinned
///        to each other, value by value.

#include <cstdint>
#include <cstring>

#include "flight_core/flight_core.hpp"
#include "flight_core/throw_detector.hpp"
#include "flight_core/types.hpp"
#include "protocol/envelope.hpp"

namespace mark4
{
    static_assert(static_cast<int>(FlightPhase::IDLE) == mark4_FlightPhase_PHASE_IDLE);
    static_assert(static_cast<int>(FlightPhase::ALTITUDE_AUTO) ==
                  mark4_FlightPhase_PHASE_ALTITUDE_AUTO);
    static_assert(static_cast<int>(FlightPhase::ARMED) == mark4_FlightPhase_PHASE_ARMED);
    static_assert(static_cast<int>(FlightPhase::BALLISTIC) == mark4_FlightPhase_PHASE_BALLISTIC);
    static_assert(static_cast<int>(FlightPhase::RECOVERY) == mark4_FlightPhase_PHASE_RECOVERY);
    static_assert(static_cast<int>(FlightPhase::HOVER) == mark4_FlightPhase_PHASE_HOVER);
    static_assert(static_cast<int>(FlightPhase::CUTOFF) == mark4_FlightPhase_PHASE_CUTOFF);
    static_assert(static_cast<int>(FlightPhase::MANUAL) == mark4_FlightPhase_PHASE_MANUAL);
    static_assert(static_cast<int>(FlightPhase::FAULT) == mark4_FlightPhase_PHASE_FAULT);
    static_assert(static_cast<int>(ThrowState::IDLE) == mark4_ThrowState_THROW_IDLE);
    static_assert(static_cast<int>(ThrowState::THRUST) == mark4_ThrowState_THROW_THRUST);
    static_assert(static_cast<int>(ThrowState::BALLISTIC) == mark4_ThrowState_THROW_BALLISTIC);
    static_assert(static_cast<int>(PilotMode::MANUAL) == mark4_RcMode_RC_MANUAL);
    static_assert(static_cast<int>(PilotMode::ALTITUDE_AUTO) == mark4_RcMode_RC_ALTITUDE_AUTO);

    /// @brief Packs one step into a Telemetry message. The truth field is
    ///        left absent: a sim composition fills it from its plant.
    /// @param frame sensor frame that entered the core
    /// @param actuators actuator frame the core produced for it
    /// @param core flight core, source of the estimated state
    /// @param[out] out message to fill, zeroed first
    inline void packTelemetry(const SensorFrame &frame,
                              const ActuatorFrame &actuators,
                              const FlightCore &core,
                              mark4_Telemetry &out)
    {
        out = mark4_Telemetry_init_zero;
        out.timestamp_us = frame.timestampUs;
        std::memcpy(out.gyro_rad_s, frame.gyroRadS.data(), sizeof(out.gyro_rad_s));
        const Quaternion &q = core.attitude();
        out.attitude_quat[0] = q.w;
        out.attitude_quat[1] = q.x;
        out.attitude_quat[2] = q.y;
        out.attitude_quat[3] = q.z;
        const std::array<float, 3> bias = core.gyroBiasRadS();
        std::memcpy(out.gyro_bias_rad_s, bias.data(), sizeof(out.gyro_bias_rad_s));
        std::memcpy(out.motor, actuators.motor.data(), sizeof(out.motor));
        out.altitude_m = core.altitudeM();
        out.baro_altitude_m = core.baroAltitudeM();
        out.vertical_velocity_mps = core.verticalVelocityMps();
        out.flight_phase = static_cast<mark4_FlightPhase>(core.flightPhase());
        out.imu_valid = frame.imuValid;
        out.baro_valid = frame.baroValid;
        const ThrowDetector &detector = core.throwDetector();
        out.throw_state = static_cast<mark4_ThrowState>(detector.state());
        out.throw_count = detector.throwCount();
        out.release_velocity_mps = detector.releaseVelocityMps();
        out.apex_timestamp_us = detector.apexTimestampUs();
        out.apex_altitude_m = detector.apexAltitudeM();
    }
} // namespace mark4
