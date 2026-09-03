# drone_sim

The desktop flight process: the flight core plus the sim composition of
platform services (`software/components/platform/src/sim/`), one
transport node on the shared discovery port. `main.cpp` parses the
arguments, builds `DroneSimApp`, runs it.

```sh
./software/build/desktop/drone_sim/drone_sim [--discovery-port N] [--node-id N] [--ota-dir DIR]
```

## Telemetry

The process exposes its measures on demand rather than pushing them: 76 of
them (the sensor frame, both estimators, the throw detector, the three
control loops, the mixer, the plant truth and the attitude error), listed in
`software/components/telemetry/README.md` and logged at boot by
`platform/telemetry`. `MIN_TELEMETRY_PERIOD_MS` is 2 ms here, the frame
period: the plant paces the loop at 500 Hz and a loopback datagram costs
nothing, so there is nothing faster to ask for.

What it broadcasts unasked is the `Status` report, one message every 10
frames: attitude, motors, phase, throw state and count, the two validity
flags and the plant's exact state.

## No plant needed to start

The process runs its loop at 500 Hz from the moment it starts, plant or
not. Without a plant the frames come from the platform clock with no
sensors (`imu_valid` and `baro_valid` false in the Status report): the flight
core stays idle, arming is refused, motors are zero, and everything on the
command path works (RC tracked, tuning answered, OTA against the emulated
flash, log levels). When a Godot plant appears the platform adopts it
(`sim/plant` INFO `plant <id> connected`), the frames switch to the plant's
simulated time and sensors, and the flight core is restarted on the new time
base; a throw scenario then runs as usual. When the plant goes silent for
500 ms it is dropped (`plant <id> lost`), the clock takes over and the
core is restarted again, idle. The application never knows whether a
plant is there: only the platform does.

The run hash (`SimRunStats`) covers plant frames only: frames without
sensors are not part of any run.
