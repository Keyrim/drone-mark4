# Godot simulator

Standalone Godot 4.4 project (GDScript only) that plays the role of the
drones and their environment: rigid bodies with a motor model, realistic
sensor models, and a transport node that talks to the flight processes. It
runs on the HOST, not in the devcontainer, and it never links flight-core:
it only speaks the wire of `software/components/protocol/mark4.proto`,
through the GDScript codec the desktop build generates into `scripts/gen/`,
inside the frames of `software/components/transport/` (see "Transport").

The plant is one node of the LAN, kind `plant`, and hosts **one virtual
drone per `drone_sim` node it hears**: a flight process that starts is
found by its beacon and gets a drone spawned on the ground for it, a flight
process that dies has its drone removed once its node expires. Each virtual
drone sends what an IMU and a barometer would measure to its own flight
process, receives the four motor commands that process decides, and
applies them as forces. Nothing else crosses the boundary.

## Run

1. Install Godot 4.4 or newer (standard build, no C# needed). The project
   already selects Jolt Physics and a 500 Hz physics tick, there is nothing to
   configure.
2. Start one flight process per drone wanted, in the container, in any
   order relative to this project (each one is a transport node on
   udp/47820 and beacons once per second):

   ```sh
   ./software/build/desktop/drone_sim/drone_sim
   ./software/build/desktop/drone_sim/drone_sim   # a second drone, optional
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

The overlay in the top left corner shows this plant's node id, the wire
hash this build speaks, how many drones it hosts, and for the followed
drone (the first one spawned, then the oldest survivor): the node id of its
flight process, the simulated time, the motor commands coming back, the
accelerometer magnitude in g, the altitude and the packet counters. With no
flight process on the LAN the world is empty and the overlay says so. The
camera, the arena fade and the world keys follow the same drone.

Every sensor frame also carries the exact state (attitude, position,
velocity, no sensor model) as `PlantTruth`; the flight process forwards it
inside its telemetry, so the attitude page served by the hub draws the
estimated attitude against the exact one, per drone. Wire vectors use the
drone frame convention of the wire (body x forward, y left, z up); the
remap from the Godot axes happens in `sim_link.gd`.

## Controls

| Key        | Effect                                                        |
| ---------- | ------------------------------------------------------------- |
| `H`        | pick the followed drone up in the simulated hand               |
| `SPACE`    | throw the followed drone (a hand throw when it is held)        |
| `R`        | reset the followed drone to its start pose, at rest, motors stopped |
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

A scripted run arrives as a `SimScenario` envelope, unicast by the flight
process to this plant: it receives it on its command receiver and forwards
it to the virtual drone that belongs to it, as its own message. One scenario is one run. It opens with a
reset - teleport, reseed every generator, clear the hand - and everything
it asks for afterwards
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

## Transport

`scripts/transport/transport.gd` (`Mark4Transport`) is the GDScript port
of `software/components/transport/`, the same frames and the same rules:
an 11-byte little-endian header (`src u32, dst u32, seq u16, hops u8`)
in front of every payload, a node table learnt from every frame heard
(address, last sequence, received / lost / duplicate counters, 3 s
expiry, `node_up` / `node_down` signals), a beacon broadcast every second
and unicast once to every newcomer, duplicates dropped by `(src, seq)`.
No relay. The node id is a random nonzero `u32` drawn at start.

Sockets, as in the C++ `UdpLink`: one shared discovery socket on
udp/47820 (`--discovery-port N` after `--` for a batch pair) that receives
the broadcasts of the LAN, and one ephemeral data socket every frame
leaves from, so its source port is this node's unicast address. Godot's
`PacketPeerUDP.bind()` sets no reuse option and refuses a port another
process already holds, so the discovery socket is a `UDPServer`: its
`listen()` sets `SO_REUSEADDR`, which Linux honours for UDP next to the
`SO_REUSEADDR + SO_REUSEPORT` pair the C++ link sets, and the hub, the
flight processes and this plant share udp/47820 on one host. The server
hands datagrams out as one `PacketPeerUDP` per remote address; the
transport keeps them and drains them all on every poll. Broadcasts go to
`255.255.255.255`, and to `127.255.255.255` when the host has no route
for the former, like the C++ link.

`scripts/transport/announce.gd` builds this plant's beacon (an `Announce`
of kind `PLANT`, name `godot-plant`, mcu `SIM`, the wire hash of the
generated codec) and reads the kind out of everyone else's, after one look
at the first byte: the plant hears every broadcast of the LAN, the
telemetry of every flight process included, and never runs the codec on
a frame it does not want.

`scripts/drone_manager.gd` (`DroneManager`, the `Drones` node of the main
scene) owns the transport and polls it once per physics tick, before the
drones run. A payload announcing a `DRONE_SIM` node from an unknown
sender spawns `scenes/drone.tscn` for that node, on a 1 m grid (four per
row); every payload from a known node goes to that drone's `SimLink`; a
node that expired has its drone freed 3 s later, unless it comes back
before. A flight process that restarts draws a new node id, so it gets a
new drone next to the old one's spot while the old one leaves. The plant
only ever decides on the `Announce` kind, never on an address.

The wire is `software/components/protocol/mark4.proto`, the source of
truth for every consumer. The GDScript codec is generated from it by the
desktop build (target `proto_gd`, run by `cmake --build --preset desktop`;
`godot` must be on the PATH) into `scripts/gen/mark4.gd` and
`scripts/gen/wire_hash.gd`, both gitignored: a fresh checkout has to run the
desktop build once before this project can talk to anything.
`scripts/sim_link.gd` (one per virtual drone) and `announce.gd` are the
only scripts that touch the codec. Every payload is one `Envelope`:

- `SimSensor`, virtual drone to its flight process, unicast: timestamp in
  microseconds, gyro [rad/s], accelerometer [m/s^2], pressure [Pa], reset
  count, lockstep timeouts, and the exact state as `PlantTruth`. Sensors
  only: the pilot state is not a sensor reading and travels out-of-band. A
  restarted plant is recognized by its clock starting over, and by its new
  node id.
- `SimActuator`, flight process to its virtual drone, unicast: echoed
  timestamp and the 4 motor commands in [0, 1].
- `SimScenario`, flight process to its virtual drone, unicast: the run to
  play, once per scenario, taken once per change of its `sequence`.

Payloads that decode to anything else are counted as dropped and ignored;
the flight process's own beacon, unicast on first contact, is not counted.
Motor commands are clamped to [0, 1] on arrival. No port is configured
anywhere: the flight process answers to the node the sensor frame came
from, and the plant found the flight process by its beacon.

Cost: one exchange (codec plus header plus send) is about 230 us of
GDScript per drone per tick on a desktop core, the codec being nearly all
of it; the transport header and the node table add a few microseconds.

The timestamp comes from the physics tick counter divided by the tick rate,
never from a wall clock. If the host cannot keep up with 500 Hz the stream
slows down but stays continuous and reproducible.

## Lockstep

`SimLink` has a `lockstep` flag, off by default (`--lockstep` after `--`
turns it on for every drone). When it is on, the physics tick sends its
sensor frame and then pumps the transport, sleeping 20 us between polls,
until the actuator frame echoing that very timestamp arrives or
`lockstep_timeout_ms` (default 50 ms) expires, resending the frame up to 20
times; then the previous motor commands are reused and the counter shown
in the overlay increases. With several drones the ticks are serialized:
each one waits for its own flight process in turn.

This blocks the main thread, so the window stops repainting while it waits. It
is meant for deterministic runs and for stepping through the flight process in
a debugger, not for comfortable flying. Off, the simulator never waits: the
replies are drained at the beginning of the next tick, which costs one tick of
latency and is closer to what a real vehicle experiences anyway.

## End to end smoke test

In the container:

```sh
cmake --build --preset desktop
./software/build/desktop/drone_sim/drone_sim
```

In another terminal, the telemetry viewer:

```sh
./software/build/desktop/hub/hub
```

then open `http://127.0.0.1:47810` for the plots.

On the host, run the Godot project, then:

1. Check that the overlay shows one drone and that its counters for sent
   and received packets both climb. Start a second `drone_sim`: a second
   drone appears on the ground next to the first; stop it: the drone is
   removed about 6 s later (3 s of node expiry, 3 s of grace).
2. At rest on the ground the accelerometer reads about 1.00 g.
3. Press `SPACE`. During the throw the accelerometer jumps for about 120 ms,
   then drops to nearly 0 g for the whole ballistic phase, and the gyro shows
   the tumble. The plots page shows the same rates, since both tools read
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
scenes/main.tscn        ground, pylons, light, camera, drone manager, overlay
scenes/drone.tscn       one virtual drone: body, meshes, sensors, sim link, hand
shaders/ground_grid.gdshader  metric grid ground material
scripts/transport/transport.gd  the transport node: frames, node table, beacon
scripts/transport/announce.gd   this plant's beacon, the kind of everyone else's
scripts/drone_manager.gd  one virtual drone per flight process heard
scripts/drone.gd        rigid body, motor model, drag, throw, tick loop
scripts/sensors.gd      accelerometer, gyro, barometer models
scripts/sim_link.gd     sensor frames out, actuator frames and scenarios in
scripts/hand.gd         the simulated hand: hold, sway, swing, release
scripts/pilot_input.gd  keyboard world keys: hold, throw, reset, quit
scripts/camera_follow.gd  chase camera
scripts/hud.gd          on screen overlay
scripts/sim_args.gd     command line flags after --
tests/transport_check.gd   ctest smoke of the transport, no peer needed
tests/plant_link_check.gd  cross-language check driven by the C++ unit tests
```

godot-tools LSP: port 6005 is forwarded by the devcontainer.
