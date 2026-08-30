# software/components/platform/src/sim

The desktop variant: the plant (Godot) is one node of the transport, and
the sensor / actuator exchange is unicast frames between the two node ids.
`PlantLink` pumps the transport and sorts what arrives (SimSensor to the
sensor source, everything else to the command ring), `SensorSourceSim`
turns the messages into frames, `MotorSinkSim` answers them,
`SimRunTracker` hashes a run, `FirmwareStoreSim` emulates the flash.

## The platform owns the plant

`SensorSourceSim` is the single owner of "is there a plant". The
composition root never knows: `waitFrame()` always returns a frame.

- **With a plant**: its SimSensor cadence drives the loop, lockstep
  included; the frame carries the simulated time and fresh sensors
  (`imuValid` and `baroValid` true).
- **Without one**: the frame is paced by the platform clock at 500 Hz,
  carries that clock's time, zeros, and both flags false. The flight core
  integrates nothing and never arms; the command path (RC, tuning, OTA on
  the emulated flash, LogControl) keeps being served on every frame.
  `MotorSinkSim` drops the answers, counted by `droppedCount()`.

The plant is whichever node sends the first SimSensor that validates. It is
dropped after `PLANT_SILENCE_US` (500 ms) without a message (paused,
killed, restarting) and the clock takes over at once; the next plant that
speaks is adopted. Appear, loss and restart are `sim/plant` INFO lines.

Every change of driver, and a plant whose simulated clock went backwards,
changes the **time base** of the frames: `sessionCount()` rises and the
composition root restarts the flight core on it, the way it does on a
world reset. Frames without sensors carry no reset counter and never
enter the run hash: `SimRunTracker` sees plant frames only.

Chosen simple on purpose: while a plant is adopted but silent, the wait
lasts up to 500 ms before the clock takes over, so commands can be served
that late during a plant restart; with the pending slot of `PlantLink`
being one message deep, a plant running ahead of the loop (never in
lockstep) overwrites its own messages, counted by `overruns()`.
