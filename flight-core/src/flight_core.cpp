#include "flight_core/flight_core.hpp"

namespace mark4
{
    void FlightCore::step(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        ++m_stepCount;

        // Kill switch first, before any other logic: it decides the outputs
        // no matter what. Estimation is pure state and keeps tracking through
        // a kill, so the attitude is fresh when the switch is released.
        if (sensors.rc.killSwitch)
        {
            actuators.motor.fill(0.0f);
            m_attitudeEstimator.update(sensors);
            return;
        }

        m_attitudeEstimator.update(sensors);

        // TODO(tmagne): real control law. Placeholder: observable throttle passthrough.
        actuators.motor.fill(sensors.rc.throttle);
    }
} // namespace mark4
