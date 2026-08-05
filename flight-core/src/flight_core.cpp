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
        runControl(sensors, actuators);
    }

    // TODO(tmagne): flight state machine (armed, ballistic, spin-up,
    // recovery). Until then the hover stack is active whenever the throttle
    // stick is raised, which is exactly what a hover test needs.
    void FlightCore::runControl(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        if (sensors.rc.throttle < ARM_THROTTLE)
        {
            // Lowering the stick is also the rearm gesture after a cutoff.
            m_armState = ArmState::DISARMED;
            m_tiltExceededSinceUs = 0U;
            actuators.motor.fill(0.0f);
            m_rateController.reset();
            m_verticalController.reset();
            m_prevControlTimestampUs = sensors.timestampUs;
            return;
        }

        if (m_armState == ArmState::CUTOFF || cutoffTripped(sensors))
        {
            // Latched: an impact, saturated rates or an unrecoverable tilt
            // mean the hover stack must never keep pushing - a wrong vertical
            // estimate after a crash would run the motors flat out on the
            // ground. Motors stay stopped until the stick is lowered.
            m_armState = ArmState::CUTOFF;
            actuators.motor.fill(0.0f);
            m_rateController.reset();
            m_verticalController.reset();
            m_prevControlTimestampUs = sensors.timestampUs;
            return;
        }
        m_armState = ArmState::ARMED;

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

        const std::array<float, 3> rateSetpoint =
            m_attitudeController.rateSetpointRadS(m_attitudeEstimator.attitude());
        const std::array<float, 3> torqueCmd =
            m_rateController.update(rateSetpoint, sensors.gyroRadS, dt);

        // Mid stick holds the altitude, full deflection climbs or sinks at
        // STICK_VZ_RANGE_MPS.
        const float vzSetpoint = (sensors.rc.throttle - 0.5f) * 2.0f * STICK_VZ_RANGE_MPS;
        const float collective =
            m_verticalController.update(vzSetpoint, m_verticalEstimator.verticalVelocityMps(), dt);

        actuators.motor = mixMotors(collective, torqueCmd);
    }

    bool FlightCore::cutoffTripped(const SensorFrame &sensors)
    {
        if (norm3(sensors.accelMps2) > CUTOFF_ACCEL_MPS2)
        {
            return true; // impact
        }
        if (norm3(sensors.gyroRadS) > CUTOFF_GYRO_RADS)
        {
            return true; // rates near the sensor's full scale
        }

        // Estimated world up seen from the body: below the threshold the
        // drone is closer to upside down than the hover stack can handle.
        const Quaternion &q = m_attitudeEstimator.attitude();
        const float upZ = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
        if (upZ < CUTOFF_TILT_MIN_UP)
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

    void FlightCore::updateEstimators(const SensorFrame &sensors)
    {
        m_attitudeEstimator.update(sensors);
        m_verticalEstimator.update(sensors, m_attitudeEstimator.attitude());
        m_throwDetector.update(
            sensors, m_verticalEstimator.verticalVelocityMps(), m_verticalEstimator.altitudeM());
    }
} // namespace mark4
