#pragma once

/// @file
/// @brief Conversion of the flight core state into the Status message.
///        An IO adapter of the composition layer: it reads only public
///        accessors, so flight-core never sees a wire type. This is also
///        where the flight core enums and their wire counterparts are pinned
///        to each other, value by value.
///
/// Status is the small fixed report of what the drone is doing, not a
/// measurement stream: a quantity that belongs to one module rather than to
/// every consumer is registered as a telemetry measure instead (see
/// `software/components/telemetry/README.md`).

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
    static_assert(static_cast<int>(FlightPhase::LEVEL) == mark4_FlightPhase_PHASE_LEVEL);
    static_assert(static_cast<int>(ThrowState::IDLE) == mark4_ThrowState_THROW_IDLE);
    static_assert(static_cast<int>(ThrowState::THRUST) == mark4_ThrowState_THROW_THRUST);
    static_assert(static_cast<int>(ThrowState::BALLISTIC) == mark4_ThrowState_THROW_BALLISTIC);
    static_assert(static_cast<int>(PilotMode::MANUAL) == mark4_RcMode_RC_MANUAL);
    static_assert(static_cast<int>(PilotMode::ALTITUDE_AUTO) == mark4_RcMode_RC_ALTITUDE_AUTO);
    static_assert(static_cast<int>(PilotMode::LEVEL) == mark4_RcMode_RC_LEVEL);

    /// @brief Packs one step into a Status message. The truth field is
    ///        left absent: a sim composition fills it from its plant.
    /// @param frame sensor frame that entered the core
    /// @param actuators actuator frame the core produced for it
    /// @param core flight core, source of the estimated state
    /// @param rcLinkOk true when the RC fail-safe is not active, as the
    ///        composition's RcTracker reports it for this frame: the frame
    ///        itself cannot tell a fail-safe from a pilot holding the kill
    /// @param[out] out message to fill, zeroed first
    inline void packStatus(const SensorFrame &frame,
                           const ActuatorFrame &actuators,
                           const FlightCore &core,
                           bool rcLinkOk,
                           mark4_Status &out)
    {
        out = mark4_Status_init_zero;
        out.timestamp_us = frame.timestampUs;
        const Quaternion &q = core.attitude();
        out.attitude_quat[0] = q.w;
        out.attitude_quat[1] = q.x;
        out.attitude_quat[2] = q.y;
        out.attitude_quat[3] = q.z;
        std::memcpy(out.motor, actuators.motor.data(), sizeof(out.motor));
        out.flight_phase = static_cast<mark4_FlightPhase>(core.flightPhase());
        out.imu_valid = frame.imuValid;
        out.baro_valid = frame.baroValid;
        out.rc_link_ok = rcLinkOk;
        const ThrowDetector &detector = core.throwDetector();
        out.throw_state = static_cast<mark4_ThrowState>(detector.state());
        out.throw_count = detector.throwCount();
    }
} // namespace mark4
