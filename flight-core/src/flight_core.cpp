#include "flight_core/flight_core.hpp"

namespace mark4
{
    void FlightCore::step(const SensorFrame &sensors, ActuatorFrame &actuators)
    {
        ++m_stepCount;

        // Kill switch first, before any other logic.
        if (sensors.rc.killSwitch)
        {
            actuators.motor.fill(0.0f);
            return;
        }

        // TODO(tmagne): real control law. Placeholder: observable throttle passthrough.
        actuators.motor.fill(sensors.rc.throttle);
    }
} // namespace mark4
