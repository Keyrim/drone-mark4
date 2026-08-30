# flight-core

Pure static library: `FlightCore::step(const SensorFrame&, ActuatorFrame&)`,
synchronous, single-threaded, paced by the frames it is handed and never by
a clock. No allocation, no waiting, no IO. The state machine, the estimators
and the controllers are documented in their headers under
`include/flight_core/`; this page only spells out the input contract and the
sensor health policy.

## Frame contract

The platform fills one `SensorFrame` per step. The kill switch is honored
first on every frame, whatever else it carries. A frame with a NaN, an Inf or
a non-increasing timestamp is rejected as a whole and the outputs hold.

Every sensor field comes with a validity flag, `imuValid` for the gyro and
accelerometer, `baroValid` for the pressure. **Valid means a fresh
measurement acquired for this frame.** A platform that could not read the
sensor leaves the flag false and the field zero; it never replays an old
sample with a new timestamp. The flags default to false, so a frame nobody
filled is a frame without sensors.

## Sensor health policy

Evaluated in `step()` right after the kill switch and the frame checks.

| Situation | Behaviour |
| --- | --- |
| IMU invalid, motors off (IDLE, ARMED, BALLISTIC, CUTOFF, MANUAL at zero stick) | Nothing integrates: attitude, bias, altitude and the throw detector keep their state. IDLE is never left, so arming is refused. Releasing the arm switch still disarms. Valid frames resume normal operation, there is no latch. This is a board booting without its sensors, or a simulator waiting for its plant. |
| IMU invalid, motors on | The last command is held for up to `IMU_FAULT_FRAMES - 1` consecutive frames (a lone bus glitch). On the `IMU_FAULT_FRAMES`th (10, 20 ms at 500 Hz) the core enters `FlightPhase::FAULT`: every motor at zero, latched. Valid frames coming back change nothing, nor does the arm switch. The kill switch is the only exit (back to IDLE, as after any kill). |
| Baro invalid | Never a fault. The vertical estimator skips the baro correction and coasts on the integrated accelerometer; `baroAltitudeM()` stops moving. On the ground, arming is refused while it lasts. In flight, the flight continues. |

The flags are not stored by the core: the composition reads them off the
frame it stepped (the telemetry packer copies them into `Telemetry`), and
`FlightPhase::FAULT` says the rest. `imuInvalidRun()` exposes the current
streak for diagnostics.

## Tests

`software/tests/unit/test_flight_core.cpp` covers every row of the table:
idle plus invalid (no integration, arming refused, resumes), flying plus
invalid under and at the threshold (hold, then FAULT latched through valid
frames and a disarm), baro invalid on the ground and in flight.
