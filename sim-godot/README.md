# Godot simulator

Standalone Godot 4.4 project (GDScript only) that plays the role of the drone
and its environment: a rigid body with a motor model, realistic sensor models,
and a UDP link to the flight process. It runs on the HOST, not in the
devcontainer, and it never links flight-core: it only speaks the wire structs
defined in `protocol/`.

The simulator sends what an IMU and a barometer would measure, receives the
four motor commands the flight process decides, and applies them as forces.
Nothing else crosses the boundary.

## Run

1. Install Godot 4.4 or newer (standard build, no C# needed). The project
   already selects Jolt Physics and a 500 Hz physics tick, there is nothing to
   configure.
2. Start the flight process in the container, listening on UDP 47800:

   ```sh
   ./build/desktop/apps/drone_sim/drone_sim 2000000
   ```

3. On a fresh clone, import the project once so Godot generates its cache
   (`.godot/` is not committed; without it the global script classes are
   unknown and every script fails to parse). Opening the project in the
   editor does it too.

   ```sh
   godot --path sim-godot --headless --import
   ```

4. Run the simulator. The Godot binary is both the editor and the runtime;
   the command line decides which one you get:

   ```sh
   godot --path sim-godot        # run the main scene directly, no editor
   godot -e --path sim-godot     # open the project in the editor (F5 to run)
   ```

The overlay in the top left corner shows the simulated time, the session id,
the motor commands coming back from the flight process, the accelerometer
magnitude in g, the altitude and the packet counters. If the counters for
received packets stay at zero, the flight process is not answering.

The small view in the top right corner compares attitudes, orientation only:
the solid gizmo mirrors the real drone, the ghost one follows the attitude
estimated by the flight process, decoded from the telemetry mirror
(udp/47803 - Godot binds ports exclusively, so the view leaves the shared
telemetry broadcast on udp/47801 to the other tools). Any divergence between
the two is directly visible.

The simulator also broadcasts its exact state (attitude, position, velocity,
no sensor model) as `SimRawPacket` on udp/47802, so the ground station can
plot the estimated attitude against the exact one. Wire vectors use the drone
frame convention of `protocol/` (body x forward, y left, z up); the remap
from the Godot axes happens in `sim_link.gd` and `sim_raw_link.gd`.

## Controls

| Key        | Effect                                                        |
| ---------- | ------------------------------------------------------------- |
| `H`        | pick the drone up in the simulated hand                        |
| `SPACE`    | throw the drone (a hand throw when it is held)                 |
| `R`        | reset the world to its start pose, at rest, motors stopped     |
| `ESC`      | quit                                                           |

These are world keys, not piloting. This project is the plant, and it holds
no pilot state at all: the kill switch, the arm switch and the throttle are
RC, they belong between the cockpit and the flight process, and they never
pass through here.

Piloting therefore flows through the hub until the command console page
lands: connect to the hub websocket endpoint and send `rc` messages aimed at
`drone_sim`. That is the same packet, port and fail-safe a real flight uses
(500 ms of silence trips the kill), which is exactly the path worth
exercising. When the kill switch is engaged the flight process answers with
four zeros and the motors spin down with their normal lag.

The usual sequence is `R` then `SPACE`. Pressing `SPACE` while the drone is
already flying simply adds another push.

## Scenarios

A scripted run arrives as a scenario block inside the lockstep reply: the
flight process receives a `SimScenarioPacket` on its command receiver and
forwards the 58 byte block on its next actuator packet. One block is one
run. It opens with a reset - teleport, reseed every generator, clear the
hand and the raw-stream decimation - and everything it asks for afterwards
(the throw, or the grab and the swing) is scheduled from that reset tick, on
this project's own tick grid. Nothing outside has to agree on which absolute
tick a run began at, and nothing carries over from the run before.

A block is played once per change of its `sequence` byte, so a sender may
resend it freely: the lockstep handshake already guarantees the block gets
here, since a tick is not complete until its reply arrives.

## Physics

- Mass 0.65 kg, inertia set by hand to (0.0023, 0.0040, 0.0023) kg*m^2 in the
  body frame (y up), damping disabled, sleeping disabled.
- Four motors on a 45 degree X layout, 0.08 m from the center of mass,
  numbered like a 4 in 1 ESC: 0 rear right, 1 front right, 2 rear left,
  3 front left. Diagonal pairs spin in opposite directions.
- Each motor follows its command through a first order lag of 25 ms, then
  produces `6.0 N * w^2` of thrust along the body y axis and a yaw reaction
  torque of `0.012 N*m * w^2`, with `w` the lagged command in [0, 1]. Four
  motors at full speed give 24 N against 6.4 N of weight, about 3.7:1.
- A linear air drag of `-0.05 N/(m/s) * v` keeps ballistic trajectories
  plausible without pretending to model a real drag polar.
- The throw is a constant force applied for 120 ms, sized so the velocity
  increment matches the exported `throw_delta_velocity_mps` (default
  (1.5, 6.5, 0) m/s), plus an angular momentum impulse for the exported
  `throw_angular_velocity_rad_s` (default (2.0, 1.0, 6.0) rad/s). Gravity keeps
  acting during those 120 ms, so the release velocity is slightly lower than
  the requested increment. Nothing is faked in the sensors: the thrust phase
  and the free fall that follows are what the rigid body actually does.

## Sensors

Sampled once per physics tick, in this order for every channel: ideal value,
constant bias, gaussian noise, clipping to the full scale, quantization to the
LSB of a 16 bit converter.

- **Accelerometer**: specific force in the body frame, not coordinate
  acceleration. It is computed from the velocity delta of the physics step,
  `f = (v - v_previous) / dt - g`, rotated into the body frame. Contact forces
  are therefore included for free: the reading is +1 g upward while sitting on
  the ground, exactly 0 in free fall, and a few g while the hand accelerates
  the drone. Full scale +/- 16 g.
- **Gyro**: body angular rates, full scale +/- 4000 deg/s, which a tumbling
  throw can approach.
- **Barometer**: standard atmosphere,
  `p = 101325 * (1 - 2.25577e-5 * h)^5.25588`, plus noise.

Noise standard deviations, bias spread and the generator seed are exported on
the `Sensors` node. The generator is seeded explicitly and the biases are drawn
once at startup, so the same seed replays the same sensor stream.

## UDP contract

The layout is defined by `protocol/include/protocol/sim_link.hpp`, which is
the source of truth; `scripts/protocol.gd` is the single GDScript copy of
its constants and `scripts/sim_link.gd` packs with them. Both packets are
packed, little endian, version byte then type byte:

- sensor packet, 45 bytes, simulator to flight process: `u8` version, `u8`
  type, `u64` timestamp in microseconds, 3 `f32` gyro [rad/s], 3 `f32`
  accelerometer [m/s^2], `f32` pressure [Pa], `u8` reset count, `u32`
  session id, `u16` lockstep timeouts. Sensors only: the pilot state is not
  a sensor reading and travels out-of-band. The session id is drawn once per
  process start, so a restarted simulator is recognized as a new plant whose
  simulated clock starts over.
- actuator packet, 84 bytes, flight process to simulator: `u8` version,
  `u8` type, `u64` echoed timestamp, 4 `f32` motor commands in [0, 1], then
  the 58 byte scenario block at offset 26, repeated on every reply.

Datagrams with another size or another version byte are counted as dropped and
ignored. Motor commands are clamped to [0, 1] on arrival.

The simulator sends to `127.0.0.1:47800` by default (exported on the `SimLink`
node) and listens on the same socket, whose local port the operating system
picks. The flight process answers to the address the sensor packet came from,
so no port needs to be reserved on the simulator side.

The timestamp comes from the physics tick counter divided by the tick rate,
never from a wall clock. If the host cannot keep up with 500 Hz the stream
slows down but stays continuous and reproducible.

## Lockstep

`SimLink` has a `lockstep` flag, off by default. When it is on, the physics
tick sends its sensor packet and then busy polls the socket, sleeping 20 us
between polls, until the actuator packet arrives or `lockstep_timeout_ms`
(default 50 ms) expires; on timeout the previous motor commands are reused and
the counter shown in the overlay increases.

This blocks the main thread, so the window stops repainting while it waits. It
is meant for deterministic runs and for stepping through the flight process in
a debugger, not for comfortable flying. Off, the simulator never waits: the
replies are drained at the beginning of the next tick, which costs one tick of
latency and is closer to what a real vehicle experiences anyway.

## End to end smoke test

In the container:

```sh
cmake --build --preset desktop
./build/desktop/apps/drone_sim/drone_sim 2000000
```

In another terminal, the telemetry viewer:

```sh
cd tools/ground-station
pipenv install
pipenv run ./ground_station.py
```

On the host, run the Godot project, then:

1. Check that the overlay counters for sent and received packets both climb.
2. At rest on the ground the accelerometer reads about 1.00 g.
3. Press `SPACE`. During the throw the accelerometer jumps for about 120 ms,
   then drops to nearly 0 g for the whole ballistic phase, and the gyro shows
   the tumble. The ground station plots the same rates, since both tools read
   the same flight process.
4. Engage the kill switch from the hub and check that the motor commands
   returned by the flight process fall to zero.
5. Press `R` to put the drone back on the ground and start again.

## Layout

The ground carries a world-space metric pattern (1 m checker, grid lines
every meter, thicker lines every 5 m) and orange pylons of known height
(1 m at the four +/-5 m corners, 2 m at 10 m out) so distances, speed and
altitude can be judged by eye. Spacing, line widths and colors are uniforms
on the ground material.

```
project.godot          engine configuration, Jolt, 500 Hz, key bindings
scenes/main.tscn        ground, pylons, light, camera, drone, overlay
shaders/ground_grid.gdshader  metric grid ground material
scripts/drone.gd        rigid body, motor model, drag, throw, tick loop
scripts/sensors.gd      accelerometer, gyro, barometer models
scripts/sim_link.gd     UDP packets in and out, scenario decode
scripts/pilot_input.gd  keyboard world keys: hold, throw, reset, quit
scripts/camera_follow.gd  chase camera
scripts/hud.gd          on screen overlay
```

godot-tools LSP: port 6005 is forwarded by the devcontainer.
