/// @file
/// @brief Minimal firmware: compiles and links for the target, does not boot
///        a board yet. Idles on default frames (kill switch engaged, so the
///        flight core keeps motors at zero).

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"

int main()
{
    mark4::FlightCore core;
    mark4::SensorFrame frame;
    mark4::ActuatorFrame actuators;

    for (;;)
    {
        core.step(frame, actuators);
    }
}
