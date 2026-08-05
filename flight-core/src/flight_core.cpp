#include "flight_core/flight_core.hpp"

#include "flight_core/mixer.hpp"

namespace mark4
{
    namespace
    {
        constexpr float US_PER_S = 1e6f;
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
            actuators.motor.fill(0.0f);
            m_rateController.reset();
            m_verticalController.reset();
            m_prevControlTimestampUs = sensors.timestampUs;
            return;
        }

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

    void FlightCore::updateEstimators(const SensorFrame &sensors)
    {
        m_attitudeEstimator.update(sensors);
        m_verticalEstimator.update(sensors, m_attitudeEstimator.attitude());
        m_throwDetector.update(
            sensors, m_verticalEstimator.verticalVelocityMps(), m_verticalEstimator.altitudeM());
    }
} // namespace mark4
