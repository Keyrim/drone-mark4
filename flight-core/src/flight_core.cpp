#include "flight_core/flight_core.hpp"

#include <cmath>
#include <cstddef>

#include "flight_core/mixer.hpp"

namespace mark4
{
    namespace
    {
        constexpr float US_PER_S = 1e6f;

        /// @return euclidean norm of a 3-vector
        float norm3(const std::array<float, 3> &v)
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }

        /// @return true when every float field of the frame is finite
        bool isFrameFinite(const SensorFrame &frame)
        {
            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                if (!std::isfinite(frame.gyroRadS[axis]) || !std::isfinite(frame.accelMps2[axis]))
                {
                    return false;
                }
            }
            return std::isfinite(frame.baroPa) && std::isfinite(frame.rc.throttle);
        }
    } // namespace

    void FlightCore::step(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        ++m_stepCount;
        actuators.timestampUs = sensors.timestampUs;

        // Kill switch first, before any other logic: it decides the outputs
        // no matter what. Estimation is pure state and keeps tracking through
        // a kill, so the attitude is fresh when the switch is released. A
        // kill also ends the mission: back to IDLE, so releasing the switch
        // never resumes an autonomous flight and flying again takes a
        // deliberate rearm.
        if (sensors.rc.killSwitch)
        {
            actuators.motor.fill(0.0f);
            m_lastMotor = actuators.motor;
            if (isFrameFinite(sensors) && acceptsTimestamp(sensors.timestampUs))
            {
                updateEstimators(sensors, deriveDt(sensors.timestampUs));
            }
            resetMission();
            return;
        }

        if (!isFrameFinite(sensors))
        {
            // NaN or Inf anywhere: the frame is garbage as a whole. NaN would
            // sail through every cutoff comparison and reach the motors, so
            // nothing downstream may ever see it.
            ++m_invalidFrameCount;
            actuators.motor = m_lastMotor;
            return;
        }

        if (!acceptsTimestamp(sensors.timestampUs))
        {
            // Out-of-order frame: a transport artifact, not a measurement.
            // Ignored entirely so no timer ever sees time move backward.
            ++m_staleFrameCount;
            actuators.motor = m_lastMotor;
            return;
        }

        const float dt = deriveDt(sensors.timestampUs);
        updateEstimators(sensors, dt);
        advancePhase(sensors);
        runControl(sensors, dt, actuators);
        m_lastMotor = actuators.motor;
    }

    bool FlightCore::acceptsTimestamp(std::uint64_t timestampUs) const
    {
        return !m_hasPrevTimestamp || timestampUs > m_prevTimestampUs;
    }

    void FlightCore::resetMission()
    {
        m_phase = FlightPhase::IDLE;
        m_rateController.reset();
        m_verticalController.reset();
        m_tiltExceeded = false;
        m_brakeDone = false;
    }

    float FlightCore::deriveDt(std::uint64_t timestampUs)
    {
        float dt = 0.0f;
        if (m_hasPrevTimestamp)
        {
            dt = static_cast<float>(timestampUs - m_prevTimestampUs) / US_PER_S;
            if (dt > MAX_STEP_S)
            {
                dt = 0.0f; // gap in the stream: re-arm without integrating
            }
        }
        m_prevTimestampUs = timestampUs;
        m_hasPrevTimestamp = true;
        return dt;
    }

    void FlightCore::advancePhase(const SensorFrame &sensors)
    {
        // Hysteresis on the stick-down boundary: between the two thresholds
        // the stick keeps its previous state, so RC noise cannot flip the
        // arming and takeover gestures at frame rate.
        if (sensors.rc.throttle < ARM_THROTTLE)
        {
            m_stickDown = true;
        }
        else if (sensors.rc.throttle > STICK_UP_THROTTLE)
        {
            m_stickDown = false;
        }
        const bool stickDown = m_stickDown;
        switch (m_phase)
        {
            case FlightPhase::IDLE:
                if (!m_verticalEstimator.ready())
                {
                    // No mission before the vertical estimate runs. The baro
                    // reference only captures at rest, so a core rebooted
                    // mid-air stays here, motors off, until the drone is back
                    // on the ground with a fresh reference.
                    break;
                }
                if (sensors.rc.armSwitch)
                {
                    // Anything detected before this instant is not a throw to fly:
                    // only a signature seen while armed may ever spin the motors.
                    m_handledThrowCount = m_throwDetector.throwCount();
                    m_phase = FlightPhase::ARMED;
                }
                else if (!stickDown)
                {
                    // Checked on entry too, so a takeoff attempt under already
                    // absurd sensor readings never powers the motors at all.
                    const bool cutoff = ImpactTripped(sensors) || GyroSaturated(sensors) ||
                                        tiltCutoffConfirmed(sensors);
                    m_phase = cutoff ? FlightPhase::CUTOFF : FlightPhase::ALTITUDE_AUTO;
                }
                break;
            case FlightPhase::ALTITUDE_AUTO:
                if (stickDown)
                {
                    // Lowering the stick is also the rearm gesture after a cutoff.
                    m_phase = FlightPhase::IDLE;
                }
                else if (ImpactTripped(sensors) || GyroSaturated(sensors) ||
                         tiltCutoffConfirmed(sensors))
                {
                    m_phase = FlightPhase::CUTOFF;
                }
                break;
            case FlightPhase::ARMED:
                if (!sensors.rc.armSwitch)
                {
                    m_phase = FlightPhase::IDLE;
                }
                else if (m_throwDetector.throwCount() > m_handledThrowCount)
                {
                    m_handledThrowCount = m_throwDetector.throwCount();
                    m_phase = FlightPhase::BALLISTIC;
                }
                break;
            case FlightPhase::BALLISTIC:
                if (!sensors.rc.armSwitch)
                {
                    m_phase = FlightPhase::IDLE;
                }
                else if (GyroSaturated(sensors))
                {
                    // The gyro is near its full scale: the attitude estimate is
                    // lost and spinning up would fly blind. Fall inert instead.
                    m_phase = FlightPhase::CUTOFF;
                }
                else if (m_throwDetector.state() != ThrowState::BALLISTIC)
                {
                    // The free fall ended before the spin-up instant (caught,
                    // landed, bounced): no motor ever turned, back to waiting.
                    m_phase = FlightPhase::ARMED;
                }
                else if (sensors.timestampUs + SPINUP_LEAD_US >= m_throwDetector.apexTimestampUs())
                {
                    m_phase = FlightPhase::RECOVERY;
                    m_recoveryStartUs = sensors.timestampUs;
                }
                break;
            case FlightPhase::RECOVERY:
                if (!sensors.rc.armSwitch)
                {
                    m_phase = FlightPhase::IDLE;
                }
                else if (ImpactTripped(sensors) || GyroSaturated(sensors) ||
                         sensors.timestampUs - m_recoveryStartUs > RECOVERY_TIMEOUT_US)
                {
                    // No tilt cutoff here: being tilted is what a recovery is.
                    // The timeout bounds a recovery that never levels instead.
                    m_phase = FlightPhase::CUTOFF;
                }
                else if (estimatedUpZ() >= RECOVERED_MIN_UP)
                {
                    m_phase = FlightPhase::HOVER;
                    m_hoverStartUs = sensors.timestampUs;
                    m_brakeDone = false;
                }
                break;
            case FlightPhase::HOVER:
                if (!sensors.rc.armSwitch)
                {
                    m_phase = FlightPhase::IDLE;
                }
                else if (ImpactTripped(sensors) || GyroSaturated(sensors) ||
                         tiltCutoffConfirmed(sensors))
                {
                    m_phase = FlightPhase::CUTOFF;
                }
                else if (!stickDown)
                {
                    // Raising the stick is the takeover gesture: from here the
                    // throttle commands the vertical velocity as in stick flight.
                    m_phase = FlightPhase::ALTITUDE_AUTO;
                }
                break;
            case FlightPhase::CUTOFF:
                // Latched: an impact, saturated rates or an unrecoverable tilt
                // mean the stack must never keep pushing - a wrong vertical
                // estimate after a crash would run the motors flat out on the
                // ground. Motors stay stopped until both controls are released.
                if (stickDown && !sensors.rc.armSwitch)
                {
                    m_phase = FlightPhase::IDLE;
                }
                break;
        }
    }

    void FlightCore::runControl(const SensorFrame &sensors, float dt, ActuatorFrame &actuators)
    {
        switch (m_phase)
        {
            case FlightPhase::IDLE:
            case FlightPhase::ARMED:
            case FlightPhase::BALLISTIC:
            case FlightPhase::CUTOFF:
                actuators.motor.fill(0.0f);
                m_rateController.reset();
                m_verticalController.reset();
                m_tiltExceeded = false;
                return;
            case FlightPhase::RECOVERY: {
                // Attitude first: the rate loop levels the drone while the
                // collective scales with the estimated tilt. A full hover
                // collective while tilted converts thrust into horizontal
                // momentum the hover then has to brake away (a 2.9 m/s throw
                // was measured leaving its recovery at 5.6 m/s), and pushes
                // toward the ground when inverted. The floor keeps torque
                // authority (torque scales with the motor commands).
                const std::array<float, 3> rateSetpoint =
                    m_attitudeController.rateSetpointRadS(m_attitudeEstimator.attitude());
                const std::array<float, 3> torqueCmd =
                    m_rateController.update(rateSetpoint, sensors.gyroRadS, dt);
                float collective = m_verticalController.hoverCollective() * estimatedUpZ();
                collective =
                    collective < RECOVERY_MIN_COLLECTIVE ? RECOVERY_MIN_COLLECTIVE : collective;
                actuators.motor = mixMotors(collective, torqueCmd);
                return;
            }
            case FlightPhase::ALTITUDE_AUTO:
            case FlightPhase::HOVER: {
                // A post-throw hover briefly leans against the dead reckoned
                // momentum of the throw, then levels for good: the braking is
                // one-shot, because once it is spent the estimate rebuilds
                // from its own bias, not from a velocity, and chasing that
                // phantom would push the real drone around forever. Stick
                // flight stays a pure leveling.
                if (m_phase == FlightPhase::HOVER && !m_brakeDone)
                {
                    const std::array<float, 2> &vh = m_verticalEstimator.horizontalVelocityMps();
                    if (vh[0] * vh[0] + vh[1] * vh[1] < BRAKE_DONE_MPS * BRAKE_DONE_MPS ||
                        sensors.timestampUs - m_hoverStartUs >= BRAKE_WINDOW_US)
                    {
                        m_brakeDone = true;
                    }
                }
                const bool braking = m_phase == FlightPhase::HOVER && !m_brakeDone;
                const std::array<float, 3> rateSetpoint =
                    braking ? m_attitudeController.rateSetpointRadS(m_attitudeEstimator.attitude(),
                                                                    brakeUpWorld())
                            : m_attitudeController.rateSetpointRadS(m_attitudeEstimator.attitude());
                const std::array<float, 3> torqueCmd =
                    m_rateController.update(rateSetpoint, sensors.gyroRadS, dt);

                // Mid stick holds the altitude, full deflection climbs or sinks
                // at STICK_VZ_RANGE_MPS. A post-throw hover ignores the stick
                // and holds until the pilot takes over.
                const float vzSetpoint = m_phase == FlightPhase::HOVER
                                             ? 0.0f
                                             : StickVerticalVelocityMps(sensors.rc.throttle);
                const float collective = m_verticalController.update(
                    vzSetpoint, m_verticalEstimator.verticalVelocityMps(), dt);

                actuators.motor = mixMotors(collective, torqueCmd);
                return;
            }
        }
    }

    float FlightCore::StickVerticalVelocityMps(float throttle)
    {
        // Deflection measured from the edge of the deadband, not from the
        // centre: the map is continuous at the deadband edge (it leaves at
        // 0 m/s), and full stick still reaches exactly STICK_VZ_RANGE_MPS.
        const float error = throttle - STICK_CENTER;
        const float magnitude = error < 0.0f ? -error : error;
        if (magnitude <= STICK_CENTER_DEADBAND)
        {
            return 0.0f;
        }
        const float scaled = (magnitude - STICK_CENTER_DEADBAND) /
                             (STICK_CENTER - STICK_CENTER_DEADBAND) * STICK_VZ_RANGE_MPS;
        return error < 0.0f ? -scaled : scaled;
    }

    bool FlightCore::ImpactTripped(const SensorFrame &sensors)
    {
        return norm3(sensors.accelMps2) > CUTOFF_ACCEL_MPS2;
    }

    bool FlightCore::GyroSaturated(const SensorFrame &sensors)
    {
        return norm3(sensors.gyroRadS) > CUTOFF_GYRO_RADS;
    }

    bool FlightCore::tiltCutoffConfirmed(const SensorFrame &sensors)
    {
        // Below the threshold the drone is closer to upside down than the
        // hover stack can handle; the confirmation filters transients.
        if (estimatedUpZ() < CUTOFF_TILT_MIN_UP)
        {
            if (!m_tiltExceeded)
            {
                m_tiltExceeded = true;
                m_tiltExceededSinceUs = sensors.timestampUs;
            }
            else if (sensors.timestampUs - m_tiltExceededSinceUs >= CUTOFF_TILT_CONFIRM_US)
            {
                return true;
            }
        }
        else
        {
            m_tiltExceeded = false;
        }
        return false;
    }

    std::array<float, 3> FlightCore::brakeUpWorld() const
    {
        // Thrust direction producing a deceleration of BRAKE_GAIN times the
        // horizontal velocity while still holding one g on the vertical:
        // (-k*vx, -k*vy, g), normalized, with the lean angle clamped.
        const std::array<float, 2> &vh = m_verticalEstimator.horizontalVelocityMps();
        float hx = -BRAKE_GAIN * vh[0] / GRAVITY_MPS2;
        float hy = -BRAKE_GAIN * vh[1] / GRAVITY_MPS2;
        const float lean = std::sqrt(hx * hx + hy * hy);
        if (lean > BRAKE_TILT_MAX)
        {
            hx *= BRAKE_TILT_MAX / lean;
            hy *= BRAKE_TILT_MAX / lean;
        }
        const float invNorm = 1.0f / std::sqrt(hx * hx + hy * hy + 1.0f);
        return {hx * invNorm, hy * invNorm, invNorm};
    }

    float FlightCore::estimatedUpZ() const
    {
        // Estimated world up seen from the body: the z component of the
        // body up axis rotated into the world frame.
        const Quaternion &q = m_attitudeEstimator.attitude();
        return q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
    }

    TuningStatus FlightCore::setParam(std::uint16_t id, float value)
    {
        // Armed is every phase where the core may spin the motors on its own.
        // ARMED itself counts: the drone is waiting for a throw and one
        // detection away from flying. CUTOFF does not: it is latched motors
        // off on the ground, which is precisely where retuning belongs.
        const bool armed = m_phase != FlightPhase::IDLE && m_phase != FlightPhase::CUTOFF;

        const TuningStatus status = m_tuning.set(id, value, armed);
        if (status == TuningStatus::OK)
        {
            applyParam(id, value);
        }
        return status;
    }

    TuningStatus FlightCore::getParam(std::uint16_t id, float &valueOut) const
    {
        return m_tuning.get(id, valueOut);
    }

    const TuningParam *FlightCore::paramInfo(std::size_t index) const
    {
        return m_tuning.info(index);
    }

    void FlightCore::applyParam(std::uint16_t id, float value)
    {
        switch (id)
        {
            case TUNING_ID_RATE_KP_ROLL_PITCH:
                m_rateController.setKpRollPitch(value);
                break;
            case TUNING_ID_RATE_KI_ROLL_PITCH:
                m_rateController.setKiRollPitch(value);
                break;
            case TUNING_ID_RATE_KP_YAW:
                m_rateController.setKpYaw(value);
                break;
            case TUNING_ID_RATE_KI_YAW:
                m_rateController.setKiYaw(value);
                break;
            case TUNING_ID_ATTITUDE_KP:
                m_attitudeController.setKp(value);
                break;
            case TUNING_ID_VERTICAL_KP:
                m_verticalController.setKp(value);
                break;
            case TUNING_ID_VERTICAL_KI:
                m_verticalController.setKi(value);
                break;
            case TUNING_ID_HOVER_COLLECTIVE:
                m_verticalController.setHoverCollective(value);
                break;
            case TUNING_ID_AHRS_KP:
                m_attitudeEstimator.setKp(value);
                break;
            case TUNING_ID_AHRS_KI:
                m_attitudeEstimator.setKi(value);
                break;
            case TUNING_ID_VEST_ALTITUDE_GAIN:
                m_verticalEstimator.setAltitudeGain(value);
                break;
            case TUNING_ID_VEST_VELOCITY_GAIN:
                m_verticalEstimator.setVelocityGain(value);
                break;
            default:
                // Unreachable: this runs only after the table accepted the id,
                // and every registered id has a case above. Adding a row
                // without its case here would silently store a value nothing
                // reads, which the tuning tests are there to catch.
                break;
        }
    }

    void FlightCore::updateEstimators(const SensorFrame &sensors, float dt)
    {
        // While the core deliberately accelerates (recovery, braking) the
        // specific force is thrust, not gravity: it sits near 1 g along body
        // up whatever the real tilt is, and letting Mahony ingest it would
        // erase the very tilt being corrected. Pure gyro rides through; the
        // gravity correction resumes once the hover is quasi-static.
        const bool maneuvering =
            m_phase == FlightPhase::RECOVERY || (m_phase == FlightPhase::HOVER && !m_brakeDone);
        m_attitudeEstimator.update(sensors, dt, !maneuvering);
        m_verticalEstimator.update(sensors, dt, m_attitudeEstimator.attitude());
        m_throwDetector.update(
            sensors, m_verticalEstimator.verticalVelocityMps(), m_verticalEstimator.altitudeM());
    }
} // namespace mark4
