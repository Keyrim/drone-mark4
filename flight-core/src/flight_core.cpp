#include "flight_core/flight_core.hpp"

#include <cmath>

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
    } // namespace

    void FlightCore::step(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        ++m_stepCount;

        // Kill switch first, before any other logic: it decides the outputs
        // no matter what. Estimation is pure state and keeps tracking through
        // a kill, so the attitude is fresh when the switch is released.
        if (sensors.rc.killSwitch)
        {
            actuators.motor.fill(0.0f);
            updateEstimators(sensors);
            m_rateController.reset();
            m_verticalController.reset();
            return;
        }

        updateEstimators(sensors);
        advancePhase(sensors);
        runControl(sensors, actuators);
    }

    void FlightCore::advancePhase(const SensorFrame &sensors)
    {
        const bool stickDown = sensors.rc.throttle < ARM_THROTTLE;
        switch (m_phase)
        {
            case FlightPhase::IDLE:
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
                    m_phase = cutoffTripped(sensors) ? FlightPhase::CUTOFF : FlightPhase::MANUAL;
                }
                break;
            case FlightPhase::MANUAL:
                if (stickDown)
                {
                    // Lowering the stick is also the rearm gesture after a cutoff.
                    m_phase = FlightPhase::IDLE;
                }
                else if (cutoffTripped(sensors))
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
                else if (norm3(sensors.gyroRadS) > CUTOFF_GYRO_RADS)
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
                else if (cutoffTripped(sensors, false) ||
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
                else if (cutoffTripped(sensors))
                {
                    m_phase = FlightPhase::CUTOFF;
                }
                else if (!stickDown)
                {
                    // Raising the stick is the takeover gesture: from here the
                    // throttle commands the vertical velocity as in stick flight.
                    m_phase = FlightPhase::MANUAL;
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

    void FlightCore::runControl(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        float dt = 0.0f;
        if (sensors.timestampUs > m_prevControlTimestampUs)
        {
            dt = static_cast<float>(sensors.timestampUs - m_prevControlTimestampUs) / US_PER_S;
            if (dt > MAX_CONTROL_STEP_S)
            {
                dt = 0.0f; // gap in the stream: hold the integrators
            }
        }
        m_prevControlTimestampUs = sensors.timestampUs;

        switch (m_phase)
        {
            case FlightPhase::IDLE:
            case FlightPhase::ARMED:
            case FlightPhase::BALLISTIC:
            case FlightPhase::CUTOFF:
                actuators.motor.fill(0.0f);
                m_rateController.reset();
                m_verticalController.reset();
                m_tiltExceededSinceUs = 0U;
                return;
            case FlightPhase::RECOVERY: {
                // Attitude first: a fixed hover collective gives the rate loop
                // its torque authority (torque scales with the motor commands)
                // without the vertical loop pushing on a still arbitrary tilt.
                const std::array<float, 3> rateSetpoint =
                    m_attitudeController.rateSetpointRadS(m_attitudeEstimator.attitude());
                const std::array<float, 3> torqueCmd =
                    m_rateController.update(rateSetpoint, sensors.gyroRadS, dt);
                actuators.motor =
                    mixMotors(VerticalController::DEFAULT_HOVER_COLLECTIVE, torqueCmd);
                return;
            }
            case FlightPhase::MANUAL:
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
                // (it is down) and holds until the pilot takes over.
                const float vzSetpoint =
                    m_phase == FlightPhase::HOVER
                        ? 0.0f
                        : (sensors.rc.throttle - 0.5f) * 2.0f * STICK_VZ_RANGE_MPS;
                const float collective = m_verticalController.update(
                    vzSetpoint, m_verticalEstimator.verticalVelocityMps(), dt);

                actuators.motor = mixMotors(collective, torqueCmd);
                return;
            }
        }
    }

    bool FlightCore::cutoffTripped(const SensorFrame &sensors, bool withTilt)
    {
        if (norm3(sensors.accelMps2) > CUTOFF_ACCEL_MPS2)
        {
            return true; // impact
        }
        if (norm3(sensors.gyroRadS) > CUTOFF_GYRO_RADS)
        {
            return true; // rates near the sensor's full scale
        }
        if (!withTilt)
        {
            return false;
        }

        // Below the threshold the drone is closer to upside down than the
        // hover stack can handle.
        if (estimatedUpZ() < CUTOFF_TILT_MIN_UP)
        {
            if (m_tiltExceededSinceUs == 0U)
            {
                m_tiltExceededSinceUs = sensors.timestampUs;
            }
            else if (sensors.timestampUs - m_tiltExceededSinceUs >= CUTOFF_TILT_CONFIRM_US)
            {
                return true;
            }
        }
        else
        {
            m_tiltExceededSinceUs = 0U;
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

    void FlightCore::updateEstimators(const SensorFrame &sensors)
    {
        // While the core deliberately accelerates (recovery, braking) the
        // specific force is thrust, not gravity: it sits near 1 g along body
        // up whatever the real tilt is, and letting Mahony ingest it would
        // erase the very tilt being corrected. Pure gyro rides through; the
        // gravity correction resumes once the hover is quasi-static.
        const bool maneuvering =
            m_phase == FlightPhase::RECOVERY || (m_phase == FlightPhase::HOVER && !m_brakeDone);
        m_attitudeEstimator.update(sensors, !maneuvering);
        m_verticalEstimator.update(sensors, m_attitudeEstimator.attitude());
        m_throwDetector.update(
            sensors, m_verticalEstimator.verticalVelocityMps(), m_verticalEstimator.altitudeM());
    }
} // namespace mark4
