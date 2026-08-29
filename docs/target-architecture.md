# Target design - functional goals and the architecture that serves them

Status: agreed target picture, not current state. This document describes
where the system is headed and why; it deliberately contains no inventory
of the existing code and no migration plan. Companions:
`docs/architecture.md` (flight system fundamentals),
`docs/tooling-architecture.md` (tooling: current state and hub proposal).

## 1. Mission

A 5-inch drone is thrown by hand with motors off. The firmware detects the
throw, predicts the apex, spins up, recovers attitude and settles into a
stable hover, where the pilot takes over. Everything below serves that
loop: flying it, simulating it, understanding it, tuning it.

---

## 2. Functional target

### 2.1 Piloting modes

Two throttle interpretations, chosen before arming and locked while armed
(changing mode requires disarming):

- **Manual**: throttle minimum means motors at zero; the stick commands
  thrust directly.
- **Altitude auto**: the stick commands target vertical velocity; stick
  centered (50%) means zero vertical speed, i.e. hold altitude.

The throw sequence requires altitude-auto mode with the throttle centered
(within a deadband) to arm. Rationale: recovery ends in a stabilized hover
(vertical speed zero), so the pilot resumes control through a command that
is continuous with the drone's actual state - no handoff discontinuity by
construction.

Every threshold in the arming and mode logic carries hysteresis or a
deadband; no boundary may chatter at frame rate.

The kill switch is unchanged and non-negotiable: a frame field, handled
first in `step()`, defaulting to safe, with RC silence meaning kill.

### 2.2 In-flight robustness

- `step()` has a defined contract for invalid input: NaN/Inf fields,
  non-monotonic timestamps and implausible sensor values (a zeroed baro
  above all) must have specified, tested behavior - never silent
  propagation to the motors.
- The RC fail-safe logic runs through the same code path in simulation
  and on the board, so every simulated flight exercises the code that
  guards the real one.
- On target: a watchdog covers the flight loop, fault handlers stop the
  motors before anything else, and bus errors escalate through a policy
  (bounded stale-data tolerance, then fail-safe) instead of silently
  reusing the last sample.

### 2.3 Dynamic tuning

Controller and estimator gains are tunable at runtime, without
reflashing:

- flight-core owns a static parameter table (id, value, bounds); no
  dynamic allocation.
- The protocol exposes set / get / list with acknowledgement; changes
  apply between two `step()` calls, never during one.
- Each parameter declares whether it may change while armed (rate gains
  during bring-up: yes; safety thresholds: no).
- The ground side owns profiles: the hub stores named parameter sets and
  pushes them to the board on connection. On-board flash persistence is
  optional and can come later.
- The same table is the entry point for offline work: batch campaigns
  vary parameters through it, without recompiling.

### 2.4 Simulation

The simulator is a physics plant, not a cockpit and not a dashboard. Three
distinct guarantees, in increasing order of ambition:

- **Determinism (lockstep)**: the simulator advances only after the
  flight process answers; runs are exactly repeatable step by step.
- **Reproducibility**: same seed + same build = same trajectory. Batch
  results carry a trajectory hash so reproducibility is verified, not
  assumed. Requirements: scenario commands travel in-band on the lockstep
  link (tick-stamped), the sensor-noise RNG reseeds per run from the run
  seed, and a run with lockstep timeouts is flagged or failed, never
  silently free-running.
- **Fidelity**: the plant model (mass, inertia, later thrust and drag)
  matches the real airframe well enough that a controller tuned in
  simulation behaves the same on the real drone. The verification loop:
  fly a real maneuver, drive the simulated plant with the actuator
  outputs it produced, and compare the simulated sensor traces against
  the ones the board reported. This works before a full airframe exists -
  a bare board can be shaken and thrown onto a mattress, modeled and
  matched.

Fidelity is a first-class, tooled function: the compare view is a
standard page, and model parameters are data, not code.

### 2.5 Ground operations

- **One command per scenario**: a launcher starts the right set of
  processes with consistent settings (simulated flight, real board,
  batch campaign). Batch campaigns use the same launcher as interactive
  sessions.
- **No hand-wired ports**: flight processes announce themselves; the
  ground side discovers whoever is alive and reconnects on its own.
- **Live monitoring**: plots of estimated state against ground truth
  (sim) or against nothing (real flight, until a reference exists),
  attitude in 3D, link health (sequence-derived loss rates).
- **Operations**: command console (arm, reset, reboot), RC piloting,
  tuning profiles.
- **Anywhere**: the same pages render in a browser, in the editor, or on
  a field laptop next to the drone; no editor required to operate.

---

## 3. Architecture

### 3.1 The dependency rule

One rule generates most of the structure, stated from the inside out:

- **flight-core depends on nothing.** Not on platform implementations,
  not on protocol/. The telemetry packer is an IO adapter and lives in
  platform; flight-core exposes state through
  accessors and never sees a wire layout. It rebuilds only when the
  algorithms change.
- **platform speaks protocol/** to move frames, telemetry and commands
  across process and transport boundaries.
- **External processes speak only protocol/** (simulator, bridges), over
  UDP or UART, and never link flight code.
- **Humans speak to the hub.** The hub is the single process that decodes
  the binary protocol on behalf of people; everything above it is JSON
  over one WebSocket endpoint.

```mermaid
flowchart LR
    subgraph flight["Flight executables - one composition each"]
        FW["firmware<br/>STM32F405, SPI sensors"]
        FWH["firmware_hil<br/>STM32F405, UART frames"]
        DS["drone_sim"]
    end

    GODOT["Godot<br/>physics plant only"]

    subgraph hub["hub daemon - links protocol/, never flight-core"]
        TR["transports<br/>UDP / UART"]
        DISC["discovery<br/>(announce packets)"]
        SVC["commands / rc / tuning profiles"]
        LAUNCH["scenario launcher CLI"]
        WS["WebSocket + JSON endpoint"]
        TR --- DISC --- SVC --- WS
    end

    subgraph ui["UI pages - no protocol knowledge"]
        P1["plots: live,<br/>real vs sim"]
        P2["command console<br/>+ tuning"]
        P3["3D attitude"]
    end

    GODOT <-->|"binary sim link, lockstep,<br/>in-band scenario commands"| DS
    DS <-->|"binary UDP + announce"| TR
    FW <-->|"binary UART"| TR
    TR -->|"streamed sensor frames (HIL)"| FWH
    LAUNCH -.->|"spawns"| DS
    LAUNCH -.->|"spawns"| GODOT
    WS <--> P1
    WS <--> P2
    WS <--> P3

    classDef checked fill:#1e8449,color:#fff,stroke:#145a32
    classDef thin fill:#2471a3,color:#fff,stroke:#1a5276
    classDef neutral fill:#7d6608,color:#fff,stroke:#4d3c04
    class FW,FWH,DS,TR,DISC,SVC,WS,LAUNCH checked
    class P1,P2,P3 thin
    class GODOT neutral
```

### 3.2 The trust boundary at step()

Validity is layered where the knowledge is:

- **platform stamps what it knows**: a sensor read that failed (bus
  error, timeout) marks the corresponding channel invalid in the
  SensorFrame instead of shipping a stale or zeroed value silently.
- **flight-core judges what only it can**: dt and monotonicity are
  derived once at the top of `step()` and passed down (no per-module
  timestamp arithmetic); NaN/Inf are scrubbed at entry; plausibility
  gates (baro innovation, accel norm, gyro quiet) reject what a healthy
  sensor cannot produce.
- The `step()` contract documents the behavior for every invalid-input
  class, and each class has a test.

### 3.3 Protocol

One protobuf schema (`software/components/protocol/mark4.proto`) and
generated codecs: nanopb for C/C++ (desktop and STM32, no allocation,
every field bounded by `mark4.options`), godobuf for GDScript, protoc for
python. The hand-packed structs and their per-language copies are gone;
the build regenerates every codec, and the plant's is checked against the
C++ one by a headless Godot in the unit tests.

Wire format properties:

- **One `Envelope`** (a oneof over every message) per datagram or serial
  frame; nothing is ever demultiplexed by size, and there is no version
  byte.
- **Wire hash**: a 32-bit hash of the schema computed at build time
  travels in every `Announce`; the hub flags a node built on another
  schema instead of dropping it silently.
- **Source identity and sequence number** on every transport frame (the
  transport header), so multiple senders coexist and loss is measurable.
- **CRC** on serial framing; XOR-class checksums are not enough for a
  link that carries flight data.
- **Announce message**: node kind, name, chip, build identity, wire hash,
  broadcast periodically; the basis of discovery.
- **Command set**: reboot, RC with the mode field (2.1), tuning
  set/get/list with ack (2.3), the updater messages.

### 3.4 Command paths: two kinds, never mixed

- **Interactive RC and commands** travel out-of-band, from the hub to the
  flight process, through `AbsCommandReceiver` - in every composition,
  simulator included. The fail-safe (silence means kill) is therefore
  exercised in every simulated flight.
- **Scenario commands** (reset, throw, scripted arming for campaigns)
  travel in-band on the lockstep sim link, tick-stamped, so batch runs
  are reproducible by construction. Batch tooling never emits RC and
  never touches an RC port: a campaign cannot, structurally, stream
  commands at a real board on the bench.

### 3.5 The hub

One desktop C++ process, in this repo, linking the generated protocol/
codec directly (zero schema duplication, a message change breaks it at
compile time) and never linking flight-core. Roles: transports (UDP, serial), discovery,
command/RC/tuning forwarding, profile storage, and the scenario launcher
CLI. Human-facing surface: a single WebSocket + JSON
endpoint serving any number of simultaneous clients.

### 3.6 UI pages

Thin web pages over the hub endpoint: plots (live, real vs sim),
command console with tuning profiles, 3D attitude
(rendered from the telemetry quaternion; the physics engine is not a
renderer for the ground station). Pages know no ports and no wire
messages, and are individually replaceable. The editor is packaging, not
architecture: the same pages render in a browser tab, an editor webview,
or a field laptop.

### 3.7 The simulator

Godot is the plant: rigid-body physics, sensor models (noise, bias,
latency), throw scenarios, and the one latency-critical binary link -
the lockstep sensor/actuator exchange with the flight process. It parses
no telemetry, renders no dashboards and captures no pilot input; those
are hub and pages concerns. Plant parameters (mass, inertia, sensor
noise) are data, so fidelity work (2.4) edits a model, not a script.

### 3.8 Compositions: one per executable, chosen at compile time

Each executable is flight-core plus one composition of platform services,
assembled in an App class; composition is a compile-time decision, one
CMake target per composition, no runtime source switching and no internal
platform `#ifdef`. A flight binary ships no dead code and has no "wrong
source selected" failure mode.

| Executable | Sensor source | Sink / links | Purpose |
| --- | --- | --- | --- |
| `firmware` | SPI/I2C sensors | motors, UART telemetry | real flight |
| `firmware_hil` | frames over UART | motors optional, UART telemetry | on-target timing with simulated data |
| `drone_sim` | UDP sim link (lockstep) | UDP telemetry | development flights against the plant |

### 3.9 Verification woven into the architecture

The guarantees above are only real if something checks them continuously:

- protocol: one schema, generated codecs, a round-trip test of every
  message in C++ and a headless-Godot exchange against the generated
  GDScript codec.
- cross-language integration: one headless single-run batch in CI proves
  the C++/GDScript wire compatibility end to end.
- reproducibility: the trajectory hash in batch results.
- `step()` contract: a test per invalid-input class (NaN, backwards
  timestamps, baro glitches, kill mid-phase).
- the hub and pages are part of the tested surface, not a blind spot that
  replaces the previous one.
