# Design and architecture audit

Date: 2026-08-09. Scope: full repository at commit 6200098 (flight-core,
platform, protocol, apps, sim-godot, tools, build system, CI, docs).
Findings are ranked by severity within each section; every finding cites
the code it comes from. A cross-cutting summary comes first because the
most expensive problems are systemic, not local.

Spot-checked before publication: the baro clamp, the unsigned timestamp
subtractions, the RC uplink batch behavior and the drone_sim exit code
were re-verified by hand against the sources cited.

---

## 1. Cross-cutting themes

These five patterns generate most of the individual findings below. Fixing
them at the root closes many findings at once.

### T1. Facts of the system are encoded in many places at once

The single largest maintenance liability. The same fact is hand-copied and
must be edited in lockstep everywhere it appears:

- The wire layout of each packet exists in roughly 12 places across 3
  languages (C++ headers, GDScript parsers, python struct strings). See 4.1.
- `PROTOCOL_VERSION = 9` is hardcoded in 9 files. See 4.1.
- The telemetry mirror port is defined as a named constant
  (`software/components/protocol/include/protocol/telemetry.hpp:20`) but recomputed as `+2`
  arithmetic in `software/components/platform/src/sim/telemetry_sender_sim.cpp:88` and in the
  port-stride layout of `tools/batch/run_batch.py:50`.
- The telemetry decimation factor 10 exists in all three apps
  (`software/drone_sim/drone_sim_app.cpp:18`,
  `software/drone_replay/drone_replay_app.cpp:8`,
  `software/drone_firmware/firmware_app.hpp:34`).
- The dt/gap policy (max step 0.05 s, non-monotonic timestamp handling) is
  implemented three times with three diverging behaviors. See 2.2 and 2.5.
- The serial framing parser exists once in C++ and twice more in python
  (`tools/telemetry/serial_bridge.py:110`, `tools/telemetry/read_serial.py:42`).

### T2. Input validation is missing at the trust boundaries

The core hardens some inputs (accel norm gate, gyro quiet gate) but the
boundaries as a whole are porous: NaN passes every safety comparison and
reaches the motor outputs (2.3), a zeroed baro frame poisons the vertical
loop (2.1), a stray datagram redirects the sim reply target (3.4), packets
are demultiplexed by size alone with no type byte (4.1), and the state
machine does unguarded unsigned subtraction on timestamps (2.2).

### T3. Interface contracts are ambiguous, so implementations diverge

`AbsSensorSource::waitFrame` returning false means "exhausted" per the
header, "2 s idle" in the sim implementation, and "never happens" on
stm32; the two apps react in opposite ways (3.1). Service lifecycle
(init/open/constructor-only) differs per implementation (3.8). The
blackbox record crosses the process boundary but lives outside protocol/
with its own version scheme (4.4).

### T4. The on-target safety story has holes ahead of real motors

No watchdog, HardFault traps in a bare infinite loop (3.3), no I2C runtime
error escalation or recovery (3.2), the RC fail-safe path is app-inline
code never exercised in simulation (3.5), and a batch campaign can stream
arm commands at a real board on the bench (4.3). None of this bites with
`MotorSinkNull`; all of it must be closed before an ESC is connected.

### T5. CI verifies less than it appears to

clang-tidy analyzes no headers and none of the stm32/firmware sources
(5.1, 5.2), the header-only protocol/ library is never analyzed at all,
python tools have zero CI coverage (5.5), and jobs run on a mutable
`:latest` image (5.3). The green checkmark overstates what was checked.

---

## 2. flight-core

Rule compliance is genuinely clean: no clock, no allocation, no iostream,
no exceptions anywhere in the library; float-only with `f` suffixes
throughout. The problems are in robustness and ownership, not hygiene.

### 2.1 HIGH: one bad baro frame injects a massive error; the tests mask it

`software/components/flight-core/src/estimator/vertical_estimator.cpp:23` clamps pressure to
`MIN_PRESSURE_PA = 1000.0f` instead of rejecting it. A frame with
`baroPa = 0.0f` (the default of `SensorFrame`,
`software/components/flight-core/include/flight_core/types.hpp:42`) yields a baro altitude
near 26 km. With the correction gains at
`vertical_estimator.cpp:85` (altitude 2.8, velocity 4.0), a single 2 ms
glitch frame moves the altitude estimate by roughly 145 m and the velocity
estimate by roughly 207 m/s. The vertical controller rails, and the throw
detector release check (`throw_detector.cpp:66`) consumes the same
poisoned velocity. Every other input has a plausibility gate; the baro has
none, and a barometer over I2C will produce garbage frames eventually.

The test suite hides this: `feed()` in `software/tests/unit/test_flight_core.cpp:24`
never sets `baroPa`, so every FlightCore integration test flies with a
permanent 0 Pa baro, self-consistently zeroed at the clamp floor. The most
likely real sensor fault is not only untested, the suite runs inside it.

Fix direction: an innovation clamp or validity flag on the baro channel,
plus at least one test that varies `baroPa` and one that glitches it.

### 2.2 HIGH: unguarded unsigned timestamp subtraction in the state machine

Both estimators defend against non-increasing timestamps
(`attitude_estimator.cpp:15`, `vertical_estimator.cpp:47`) but the state
machine does not:

- `flight_core.cpp:117`: `sensors.timestampUs - m_recoveryStartUs` wraps
  to about 1.8e19 on an out-of-order frame and instantly latches CUTOFF
  during RECOVERY. The drone falls.
- `flight_core.cpp:213`: same wrap ends the brake window immediately.
- `flight_core.cpp:265`: same wrap fires the tilt cutoff without its
  300 ms confirmation.

Related: `runControl` (`flight_core.cpp:162`) sets dt to 0 on a backwards
stamp but still overwrites `m_prevControlTimestampUs` with the older
value, so the next frame double-counts the interval. No test feeds a
non-monotonic timestamp to FlightCore.

The deeper flaw is that monotonicity/dt policy is not owned in one place
(see 2.5); `step()` should derive dt once, defensively, and pass it down.

### 2.3 MEDIUM: NaN passes every safety gate and reaches the motors

All cutoff checks have the form `norm > THRESHOLD`
(`flight_core.cpp:245`); NaN makes every comparison false, so an insane
frame disables the protections built for insane frames. NaN gyro corrupts
the quaternion at `attitude_estimator.cpp:74` with no reset path, and
`mixer.cpp:8` `clampCommand(NaN)` returns NaN, so `ActuatorFrame::motor`
can carry NaN to the platform layer where a float-to-DShot cast is UB.
`step()` has no input scrubbing and its contract
(`flight_core.hpp:36`) says nothing about invalid frames.

### 2.4 MEDIUM: tuning ownership is unresolved

30+ compile-time constants across 7 classes, no config struct. FlightCore
has no constructor and default-constructs its six sub-modules
(`flight_core.hpp:166`), yet three of them expose gain constructors that
FlightCore can never exercise, while `RateController::update` hardcodes
its gains inside the function body (`rate_controller.cpp:23`). Several
constants describe themselves as calibration knobs for the real airframe
(`flight_core.hpp:104`, `vertical_controller.hpp:16`,
`rate_controller.hpp:18`). Monte Carlo campaigns cannot sweep any of them
without recompiling, and hardware tuning will mean editing headers. A
single tuning struct passed to the FlightCore constructor solves both.

### 2.5 MEDIUM: dt/gap policy copy-pasted three times

`AttitudeEstimator::MAX_STEP_S` (`attitude_estimator.hpp:44`),
`VerticalEstimator::MAX_STEP_S` (`vertical_estimator.hpp:42`) and
`FlightCore::MAX_CONTROL_STEP_S` (`flight_core.hpp:85`) carry the same
0.05f with three implementations and subtly different behavior: the
estimators skip the step on a gap, the control path runs with dt 0, and
only the control path lacks the backwards guard (2.2). Changing the loop
rate means reconciling all three.

### 2.6 MEDIUM: telemetry packing and Blackbox are composition-layer code

`software/components/flight-core/include/flight_core/telemetry.hpp:12` includes
`protocol/telemetry.hpp` and `software/components/flight-core/CMakeLists.txt:21` links
protocol PUBLIC. The "pure" algorithm library now rebuilds on every wire
layout change, and the "external processes speak ONLY protocol/" boundary
has an internal consumer. `packTelemetry` reads only public accessors and
belongs in apps or platform. `Blackbox` (`blackbox.hpp:12` includes
`platform/log_sink.hpp`) is never called by FlightCore, only by the apps;
it is an IO adapter parked in the algorithm library, and the accessor
surface of FlightCore is already shaped by telemetry needs rather than
control needs.

### 2.7 MEDIUM: arming boundary has no hysteresis

`flight_core.cpp:49`: `stickDown = throttle < ARM_THROTTLE` flips phase
per-frame in both directions. RC noise around the threshold chatters
MANUAL/IDLE at 500 Hz, each IDLE pass zeroing motors and resetting both
controllers (`flight_core.cpp:174`). Also a handoff discontinuity: HOVER
takeover requires only throttle at 0.05 (`flight_core.cpp:137`) but the
MANUAL stick mapping makes 0.05 mean a 1.8 m/s descent
(`flight_core.cpp:222`); the natural grab-the-drone gesture commands
nearly full descent the instant it crosses the threshold.

### 2.8 MEDIUM: blackbox records have no sync marker or CRC

`blackbox.hpp:23`: decoders split by size alone. A blackbox exists for
logs that end in a torn write (power loss on impact); with no per-record
magic and no checksum a decoder cannot resynchronize after a partial
record, and everything after the first tear is silently garbage.

### 2.9 MEDIUM: `VerticalEstimator::ready()` gates nothing

Only the unit test calls it. FlightCore enters MANUAL within the first
100 ms on zeroed estimates, and the baro reference is captured once,
unconditionally, from the first 50 frames whatever the drone is doing
(`vertical_estimator.cpp:33`); powered on mid-motion or after the in-air
reboot command, the reference is garbage with no recapture API. Either
the flag should gate the state machine or it should not exist.

### 2.10 LOW

- `timestampUs == 0` doubles as a "no streak" sentinel in three timers
  (`throw_detector.hpp:112`, `flight_core.cpp:261`) although 0 is a
  legitimate timestamp; the attitude estimator avoided the same trap with
  an explicit bool (`attitude_estimator.hpp:98`).
- The kill-switch path resets controllers and phase but not
  `m_tiltExceededSinceUs` (`flight_core.cpp:31`); the set of state that
  survives a kill is implicit rather than designed.
- Horizontal dead reckoning lives in `VerticalEstimator`
  (`vertical_estimator.hpp:96`); it exists only to feed the brake
  maneuver and has no natural home the day a flow sensor or GPS arrives.
- BALLISTIC re-implements the gyro-norm check inline
  (`flight_core.cpp:96`) because `cutoffTripped(sensors, withTilt)` cannot
  express "gyro only"; the bool parameter shows the predicate decomposes
  along the wrong axis.

### 2.11 Test gaps (high value, currently zero coverage)

Non-monotonic timestamps through FlightCore; any baro variation at all;
NaN/Inf frames; kill switch engaged mid-RECOVERY or mid-HOVER (only tested
from ARMED); the RECOVERY 2 s timeout; the BALLISTIC gyro cutoff; the
MANUAL throttle-to-vz mapping; VerticalEstimator gap and backwards
timestamp handling.

---

## 3. platform and apps

### 3.1 HIGH: `waitFrame` false means three different things

`software/components/platform/include/platform/sensor_source.hpp:18` defines false as
"exhausted". The sim implementation returns false on a transient 2 s idle
(`sensor_source_sim.cpp:27`), and EINTR is folded into "0 bytes" in
`udp_link.cpp:94`. `drone_sim` treats false as end-of-run
(`drone_sim_app.cpp:123`); `firmware` retries forever
(`firmware_app.cpp:105`) even though its source documents "always true".
Same return value, opposite semantics: the interface does not specify
behavior. Concrete failure: if the simulator never sends a packet,
drone_sim exits after 2 s with 0 steps and exit code 0
(`software/drone_sim/main.cpp` returns 0 unconditionally), while CLAUDE.md
documents exit code 0 as the health signal; a batch run that never
connected is indistinguishable from success. Needs a tri-state result (or
timeout policy moved out of the source), and main() should fail on zero
frames.

### 3.2 HIGH: I2C bus has no runtime error escalation

`software/components/platform/src/stm32/i2c_bus.cpp:75` (`waitEvent`) checks only AF (NACK);
BERR and ARLO are never tested nor cleared, and `abortTransfer`
(`i2c_bus.cpp:96`) leaves them latched, so after a line glitch every flag
wait spins its full timeout. Timeouts are raw loop counts
(`FLAG_TIMEOUT_LOOPS`, `i2c_bus.cpp:53`) whose duration varies with clock
and optimization, although a calibrated DWT cycle counter already exists
(`board.cpp:110`). `recoverBus()` (`i2c_bus.cpp:226`) runs only inside
`init()`; nothing re-runs it in flight. If the bus wedges, each 500 Hz
tick pays two full BUSY timeouts (order of milliseconds) while the core
flies on a frozen IMU sample (`sensor_source_stm32.cpp:50` silently
reuses `m_lastSample` with only a counter): unbounded stale-gyro flight
with a degraded loop rate and no escalation policy.

### 3.3 HIGH: no watchdog, no fault handling on target

`software/components/platform/src/stm32/startup.c:30`: every exception including HardFault
traps in a bare infinite loop; no IWDG/WWDG anywhere. Any fault or hang
leaves the board dead until power cycle, and with a future DShot sink the
motors would hold their last command. Tempered today by `MotorSinkNull`,
but the App pattern currently has no place where a safety net would live;
worth designing before the ESC work, not after.

### 3.4 MEDIUM: `UdpLink` latches the sender of any datagram

`udp_link.cpp:104` records `m_lastSender` before any validation;
version/size filtering happens later in `SensorSourceSim`. One stray
datagram on the sim port redirects the next motor reply away from the
simulator and stalls the lockstep. Validation and addressing live on
opposite sides of the abstraction; the sender worth latching is the one
of a validated sensor packet.

### 3.5 MEDIUM: the RC fail-safe path is never exercised in simulation

`software/drone_firmware/firmware_app.cpp:110`: packet dispatch, RcCommandPacket
decode, timeout fail-safe and frame grafting are app-inline code that
runs on target only. The sim delivers RC inside SimSensorPacket, so
AbsCommandReceiver has no sim implementation and the code path that
guards real flight is bypassed in every simulated flight. Strongest
candidate for the (currently empty) `software/components/platform/src/common/`: an RcTracker
helper testable on desktop and shared by both compositions. Dispatch is
also by (size, version) with no type byte (`firmware_app.cpp:118`), one
packet away from ambiguity.

### 3.6 MEDIUM: `software/components/platform/src/common/` is empty while the apps triplicate glue

`sendTelemetry()` is byte-identical in `drone_sim_app.cpp:186` and
`drone_replay_app.cpp:44`; the waitFrame/step/push/telemetry loop skeleton
is written three times and has already drifted (replay decimates on
`stepCount()`, sim on a local counter). The common README promises shared
composed helpers; nothing has ever landed there.

### 3.7 MEDIUM: replay broadcasts on the fixed telemetry port

`drone_replay_app.cpp:25` hardcodes TELEMETRY_PORT with no CLI override
(unlike drone_sim), so replaying a log while a sim or the serial bridge
runs interleaves two indistinguishable telemetry streams on udp/47801.
Related, system-wide: TelemetryPacket has no source identity (see 4.5).

### 3.8 LOW

- Lifecycle is inconsistent across services (init/open/constructor-only),
  and `FirmwareApp::init` initializes bus/imu/baro before m_clock although
  the class promises declaration-order init (`firmware_app.cpp:53` vs
  `firmware_app.hpp:22`); the pattern's value is that the promise is
  mechanical.
- WFI race in `SensorSourceStm32::waitFrame`
  (`sensor_source_stm32.cpp:41`): an interrupt between the tick test and
  wfi sleeps one extra frame, counted as an overrun. Classic; either use
  WFE or document the accepted race.
- `ClockStm32::nowUs` wrap extension is a non-reentrant RMW requiring a
  call at least every 71 min (`clock_stm32.cpp:29`); the constraint lives
  only in a comment and AbsClock gives no hint.
- File-scope `volatile g_ticks` silently limits SensorSourceStm32 to one
  instance (`sensor_source_stm32.cpp:24`), undocumented static coupling in
  an injectable-looking service.
- The shared UART sender funnels blackbox through the telemetry counters
  (`log_sink_uart.hpp:32`), so the degraded LED and status line cannot say
  which stream is losing data, and blackbox loss has no dedicated counter.

---

## 4. Protocol and inter-process architecture

### 4.1 HIGH: the wire format is hand-duplicated everywhere, demuxed by size

- TelemetryPacket layout: `software/components/protocol/include/protocol/telemetry.hpp`, the
  flight-core packer, `tools/ground-station/telemetry_wire.py:29` and
  `:35`, a second independent copy in `tools/telemetry/read_serial.py:16`,
  `tools/telemetry/serial_bridge.py:30`, and a hand-computed
  `QUAT_OFFSET := 21` in `sim-godot/scripts/attitude_compare.gd:19`.
- SimCommandPacket: protocol header plus magic offsets in
  `sim_command.gd:125`, `rc_uplink.gd:90` (including a pad-with-10-floats
  loop that must track the struct tail), `run_batch.py:35`,
  `serial_bridge.py:38`.
- `PROTOCOL_VERSION = 9` hardcoded in 9 files (1 C++, 5 GDScript,
  3 python).
- No packet type/ID byte anywhere; every consumer demultiplexes by
  (datagram size, version byte), e.g. `sim_link.gd:234` and the serial
  bridge separating telemetry from blackbox purely by length 95 vs 59
  (`serial_bridge.py:127`). Two packets converging to the same size become
  indistinguishable, and a drifted copy fails as a silent 100% drop.

Only C++ has compile-time layout checks; python asserts against its own
constants; GDScript offsets are checked by nothing. Fix direction: a type
byte after the version byte, plus either a small generator emitting the
.gd/.py constants from the headers or a cross-language golden-packet test
in CI.

### 4.2 HIGH: Monte Carlo seed replay is not actually reproducible

`run_batch.py:9` and `tools/batch/README.md` promise exact replay of a
failing seed; only the drawn parameters are reproduced, not the
trajectory:

- Scenario commands arrive on a separate UDP socket polled per physics
  tick (`sim_command.gd:71`); the tick at which RESET/THROW lands depends
  on host scheduling, so the same seed can diverge near decision
  boundaries.
- The sensor-noise RNG is seeded once at startup (`sensors.gd:63`) and
  never reseeded on world reset (`drone.gd:151` resets physics only), so
  the noise sequence depends on how many ticks all prior runs consumed.
- Lockstep timeouts silently degrade to reusing the last motor commands
  (`sim_link.gd:64`, `:149`), and `run_batch.py` never reads the
  `lockstep_timeouts` counter, so an overloaded host quietly produces
  partially free-running physics with no flag on the result row.

Fix direction: tick-stamped or in-band commands on the lockstep link,
reseed the sensor RNG per run from the run seed, annotate or fail runs
with nonzero lockstep timeouts.

### 4.3 HIGH: batch instances stream RC, including arm, at the real board port

`rc_uplink.gd` streams pilot state to 127.0.0.1:47805 from every
simulator instance with no headless guard (unlike `attitude_compare.gd`
which has one) and no disable flag; `run_batch.py` overrides the flight,
command and raw ports but not the RC port. Batch campaigns arm the drone
(`run_batch.py:265`), so if `serial_bridge.py` is running with a board on
the bench (its stated purpose is to be left running), N parallel headless
instances stream interleaved contradictory kill/arm/throttle states into
the real flight controller. The sim cockpit and the real-hardware cockpit
share a hardcoded localhost port with no explicit opt-in. This is the one
finding with a physical-safety flavor; close it before bench sessions and
batch campaigns coexist.

### 4.4 MEDIUM: the blackbox record crosses processes but bypasses protocol/

`serial_bridge.py:33` and `blackbox_dump.py:14` hardcode size 59 and
version 2 straight from `flight_core/blackbox.hpp`, contradicting the
rule that external tools speak only protocol/. It also creates a second
uncoordinated version scheme (blackbox v2 vs protocol v9) on the same
UART, demuxed by size alone. Move the record into protocol/ or add a type
byte on the serial link.

### 4.5 MEDIUM: no sequence numbers, no source identity on any stream

No packet carries a sequence counter, so the ground station cannot tell a
healthy 50 Hz stream from a 500 Hz stream losing 90%, and `run_batch.py`
misclassifies a lost RESET as "stalled" rather than retrying
(`run_batch.py:165`). TelemetryPacket also has no source/session ID while
two senders (drone_sim and the serial bridge) emit identical packets on
the same ports; running both, which the ghost view encourages, makes
`StreamClock` (`telemetry_wire.py:152`) interpret each cross-stream
backward timestamp as a reboot and re-anchor continuously. One sourceId
byte and one sequence byte would close both.

### 4.6 MEDIUM: flight-process handling of a Godot restart

The `resetCount` mechanism (`sim_link.hpp:36`, consumed at
`drone_sim_app.cpp:126`) covers explicit resets but a restarted Godot
starts again at reset_count 0; if it comes back within the 2 s idle
timeout, timestamps jump backward, dt clamps to 0, and the core silently
freezes with stale estimator state until sim time re-passes the old
high-water mark. Randomizing the initial resetCount (or a session ID)
closes this.

### 4.7 MEDIUM: weak serial integrity

`serial_framing.hpp:27`: XOR-8 misses even numbers of same-position bit
flips and all transpositions; the length byte itself is unprotected, so a
corrupted length swallows up to 255 bytes and can synthesize a plausible
frame before resync. Acceptable for live telemetry; questionable for the
blackbox stream, which is the forensic record. A CRC-8/16 costs nothing
at 921600 baud.

### 4.8 LOW

- Packed-struct portability is patched, not designed: memcpy workarounds
  for misaligned packed arrays already exist (`sensor_source_sim.cpp`,
  `motor_sink_sim.cpp` comments), nothing asserts little-endianness, and
  pragma pack semantics must hold for whatever compiler builds the ESP32
  bridge.
- Broadcast to 255.255.255.255 (`telemetry_sender_sim.cpp`,
  `sim_raw_link.gd:31`) sprays the LAN; some networks filter limited
  broadcast, making the ghost view fail mysteriously.
- Two attitude-error definitions: the live ground station pairs telemetry
  with the last seen raw quaternion with no time alignment
  (`ground_station.py:252`) while `stream_compare.py:54` aligns within
  30 ms; live plots show spikes the offline tool will not reproduce.
- `sim_stub.py:35` documents its actuator format as version+motors but
  the format includes the echo timestamp; exactly the silent doc/wire
  divergence T1 predicts.
- `serial_bridge.py:27` hardcodes /dev/ttyUSB0 and 921600 with no CLI
  override, and its blocking 0.5 s serial read stalls uplink relay
  precisely when the board is silent.
- Lockstep resends are re-stepped: `sensor_source_sim.cpp` has no
  duplicate-timestamp rejection, so each resend is fed to step() again
  and duplicated into blackbox and telemetry counters.
- Sensor model realism gaps that bias batch statistics optimistically: no
  motor-vibration coupling into the IMU, no sensor latency, sensors
  sampled exactly at the control tick.

---

## 5. Build system, CI, docs

### 5.1 HIGH: clang-tidy never checks headers

No `HeaderFilterRegex` in `.clang-tidy` and no `-header-filter` in
`.github/workflows/ci.yml:86`, so diagnostics located in any .hpp are
suppressed. protocol/ is header-only with zero translation units, so it
is never analyzed at all despite appearing in the CI path regex; the
naming rules the guidelines claim are linter-enforced only fire on .cpp
declarations. One line to fix, likely a backlog of latent findings behind
it.

### 5.2 HIGH: clang-tidy and desktop warnings never see stm32 or firmware

The tidy job configures only the desktop preset; `software/components/platform/src/stm32` and
`software/drone_firmware` exist only when crosscompiling
(`software/components/platform/CMakeLists.txt:8`, `software/CMakeLists.txt:1`), so none of the
~17 register-level driver sources appear in the compile database and
run-clang-tidy silently skips them. The code most in need of bugprone-*
checks has zero static analysis; the stm32 CI job only proves it
compiles.

### 5.3 MEDIUM: CI runs on the mutable :latest image

All 5 jobs pull `:latest` (`ci.yml:18` and siblings) although a
`:$GITHUB_SHA` tag is pushed and never consumed. A push touching both
.devcontainer/ and code races the image rebuild, and re-running an old CI
run months later uses whatever :latest is now, so a green history can
silently stop being reproducible. Pin jobs to the sha tag or a digest.

### 5.4 MEDIUM: Catch2 fetched from the network every run, tag not pinned

`software/tests/unit/CMakeLists.txt:3` uses FetchContent with GIT_TAG v3.8.1, a
movable tag rather than a commit SHA, cloned at configure time in three
jobs with no cache; a network hiccup fails three unrelated jobs, and a
force-pushed tag changes what CI builds with no diff in the repo.

### 5.5 MEDIUM: python tooling has zero CI coverage

Five load-bearing tools (batch, sim-stub, serial bridge, read_serial,
ground-station) get no lint, no type check, not even an import smoke
test. Godot is baked into the CI image yet nothing runs a single
`--runs 1` headless batch, so C++/GDScript/python wire compatibility can
drift silently (which is how the T1 duplication actually breaks).

### 5.6 MEDIUM: guidelines contradict the lint config

`docs/contributing/cpp-guidelines.md:50` mandates `s_camelCase` static
members; `.clang-tidy:173` requires CamelCase with no prefix; one of the
two is wrong. The guide also recommends idioms illegal in this repo
(factories returning unique_ptr, exceptions, dynamic_cast) with no
project addendum saying which sections do not apply.

### 5.7 MEDIUM: CMAKE_CROSSCOMPILING conflates "cross" with "STM32F405"

Mirrored switches in `software/components/platform/CMakeLists.txt:8` and
`software/CMakeLists.txt:1` must be edited in lockstep, and any second cross
toolchain (the ESP32 bridge is the obvious candidate) would silently
build platform_stm32. A DRONE_PLATFORM cache variable set per preset
names the variant explicitly.

### 5.8 LOW

- Firmware: no LTO (typically 10-20% flash on arm-gcc), size printed
  post-build but never tracked or budgeted in CI.
- MCU flags live only in `*_FLAGS_INIT`
  (`toolchain-arm-none-eabi.cmake:19`); a preset setting CMAKE_CXX_FLAGS
  as a cache variable (the desktop-san pattern) would silently drop the
  float ABI.
- `firmware` does not link drone_strict (`software/drone_firmware/CMakeLists.txt:6`);
  it works only because the toolchain duplicates -fno-exceptions, so the
  invariant is encoded in two unrelated places.
- desktop-san sanitizes C++ only; the first desktop .c file joins the
  build uninstrumented.
- ci.yml: container block copy-pasted 5 times, no concurrency
  cancellation, no timeout-minutes, the multi-GB image pulled even by the
  format job.
- The ASCII-only/English-only and Conventional Commits hard rules are not
  machine-enforced anywhere.
- devcontainer-image.yml rebuilds on any .devcontainer/** change although
  only the Dockerfile affects the image.

Docs drift is otherwise minimal: interfaces, ports, CLI flags, timeouts
and tool references in README/architecture/bring-up all match the code.

---

## 6. What is genuinely good

Worth saying explicitly, because these are the load-bearing decisions:

- The data-paced contract is real, not aspirational: the timestamp
  travels in the frame everywhere, the core never reads a clock, AbsClock
  stays internal to platform, and `m_handledThrowCount` snapshotted at
  arm time closes a subtle pre-arm throw race by design.
- Kill-switch semantics: handled first, estimators deliberately keep
  tracking through a kill, kill ends the mission, and the default is
  safe (`RcInput::killSwitch` defaults to true).
- The Mahony gating stack: each gate has a comment explaining the failure
  it prevents and a dedicated test; `allowAccelCorrection` shows the
  estimator/state-machine interaction was actually thought through.
- The lockstep handshake: echoing the sensor timestamp turns UDP into a
  loss-tolerant request/response, doubles as the boot-order shim in
  batch, and needed zero extra packet types. resetCount-in-frame is the
  right way to model teleports.
- The fail-safe-by-streaming RC contract (silence means kill): every
  crash, cable pull or window close along the chain is inherently safe.
  End to end, the best-engineered path in the system.
- The STM32 I2C data-phase choreography follows the RM0090 N=1/N=2/N>=3
  master-receiver sequences correctly, and recoverBus documents the real
  incident that motivated it; the UART SPSC rings have correct barrier
  placement and a drop-whole-packet policy.
- Build hygiene: the linker-script INTERFACE propagation with
  INTERFACE_LINK_DEPENDS, the flight_core_types/platform INTERFACE split,
  drone_warnings/drone_strict kept off FetchContent deps, and a textbook
  toolchain file.
- `telemetry_wire.py` as one shared, testable decoder reused by three
  tools is the pattern the rest of the tooling should follow.

---

## 7. Suggested order of attack

1. Safety-relevant core fixes, all small and testable: baro validity gate
   (2.1), timestamp monotonicity owned once in step() (2.2, 2.5), NaN
   scrubbing at the step() boundary (2.3), arming hysteresis (2.7). Add
   the missing tests from 2.11 alongside.
2. Bench safety and batch integrity before mixing hardware and campaigns:
   RC port isolation in batch (4.3), a packet type byte plus source ID
   (4.1, 4.5), seed reproducibility (4.2).
3. Contract cleanups that stop the drift: waitFrame tri-state and nonzero
   exit on zero frames (3.1), RcTracker in software/components/platform/src/common shared by
   sim and firmware (3.5, 3.6), blackbox record into protocol/ (4.4),
   telemetry packer out of flight-core (2.6).
4. CI honesty: HeaderFilterRegex (5.1), tidy over the stm32 compile
   database (5.2), pin the CI image and Catch2 (5.3, 5.4), one headless
   `--runs 1` batch smoke as the cross-language wire test (5.5).
5. Before real motors: watchdog and fault-handler design (3.3), I2C
   runtime escalation (3.2), tuning config struct for hardware bring-up
   (2.4).
