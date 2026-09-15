# telemetry

The registry of named float measures every node exposes. A leaf library
like `log`: static lib, `drone_warnings` alone, no heap, no iostream, no
exceptions, no RTTI, and it builds for the F405 as it stands. It knows
names, units and where values live; it knows nothing of the wire, of ids,
of periods or of subscribers. Those belong to the provider
(`telemetry/provider.hpp`, target `telemetry_provider`, which links
`messaging`), described at the end of this page: it freezes the list into a
table and answers the telemetry messages of `protocol/mark4.proto`.

## The model

A measure is declared where the value is computed, as a member of the
object that owns it:

```cpp
class VerticalEstimator
{
    ...
  private:
    float m_altitudeM = 0.0f;
    TelemetryEntry m_altitudeEntry{"estimator/altitude",
                                   TelemetryUnit::M,
                                   m_altitudeM};
};
```

The entry keeps a pointer to the value, so the value must outlive it -
declaring both in the same object, the value first, is what guarantees it.
Reading a measure is a pointer dereference: nothing is copied, nothing is
sampled until a subscriber asks.

The second constructor takes a context and a reader function for what is
not a plain float member: an enum, a boolean, a `std::uint64_t`, a quantity
derived from several fields.

```cpp
TelemetryEntry m_stateEntry{"throw/state", TelemetryUnit::UNITLESS, this, &ReadState};
```

Use it sparingly. A reader runs inside the sampling loop, so anything more
than a cast belongs in a float member updated once per step instead - which
is also what keeps a plotted curve consistent with the step that produced
it.

`TelemetryEntry` is non-copyable and non-movable. The constructor appends
to a process-wide intrusive list and the destructor unlinks, so objects
that come and go (a flight core rebuilt on a reset, several of them in one
test binary) leave no dangling entry behind. The list is kept in
construction order, because a wire adapter turns that order into the ids it
publishes: the same build must produce the same table on every run.

## Naming

Hierarchical lowercase paths, `area/thing`, `/` as the only separator, at
most `MAX_TELEMETRY_NAME` = 40 characters, unique by convention:

    sensor/gyro_x            estimator/altitude
    estimator/attitude/roll  rate/roll/p_term
    mixer/motor_0            sim/truth/position_z

The unit is never part of the name (`TelemetryUnit` carries it), and the
name is the stable identity of a measure across reboots: a ground tool
binds a curve to a name and routes samples by the id of the table it
pulled.

## Limits

- `MAX_TELEMETRY_NAME` = 40 characters. A longer name is refused by the
  provider with a WARN, not truncated.
- `MAX_TELEMETRY_ENTRIES` = 128. The registry itself has no limit; this is
  the size of the frozen table the provider indexes, and therefore the
  largest id that can travel. Entries past it are ignored with a WARN.
- Not thread-safe: every node registers and samples from its one loop
  thread.

## The measures

Registered today, by area and by where they live. `[r]` marks a reader
rather than a pointer. The count a node exposes is logged at boot
(`telemetry/provider`): 76 for `drone_sim`, a few less for the firmware,
which has no plant behind it.

Platform, from the frame the loop stepped
(`platform_common/frame_telemetry.hpp`, a copy of the last `SensorFrame`):

    sensor/gyro_x  sensor/gyro_y  sensor/gyro_z        rad/s
    sensor/accel_x sensor/accel_y sensor/accel_z       m/s2
    sensor/baro_pressure                              Pa
    sensor/imu_valid [r]  sensor/baro_valid [r]        0/1
    sensor/frame_dt                                   us
    rc/throttle                                       unitless

Attitude estimator (`flight_core/attitude_estimator.hpp`). The quaternion
components are read straight out of the estimate; the Euler angles and the
bias are mirrors refreshed once per update, so a curve is exactly the state
the step produced:

    estimator/attitude/w  x  y  z                     unitless
    estimator/attitude/roll  pitch  yaw               rad
    estimator/gyro_bias_x  y  z                       rad/s

Vertical estimator (`flight_core/vertical_estimator.hpp`). The fused
altitude next to the raw pressure channel it is corrected toward: the gap
between the two curves IS what the baro contributes.

    estimator/altitude  estimator/baro_altitude       m
    estimator/vertical_velocity                       m/s
    estimator/horizontal_velocity_x  y                m/s

Throw detector (`flight_core/throw_detector.hpp`):

    throw/state [r]                                   unitless
    throw/count [r]                                   count
    throw/release_velocity                            m/s
    throw/apex_altitude                               m
    throw/apex_time [r]                               us

Attitude loop (`flight_core/attitude_controller.hpp`). A pure P on the tilt
error, so there is no integral or derivative to expose; the yaw axis is not
commanded by it:

    attitude/roll/error  attitude/pitch/error         unitless (sin of the angle)
    attitude/roll/output  attitude/pitch/output       rad/s

Rate loop (`flight_core/rate_controller.hpp`), the inner loop a tuning
session is won or lost on, per axis:

    rate/{roll,pitch,yaw}/setpoint  measurement  error    rad/s
    rate/{roll,pitch,yaw}/p_term  i_term  output          unitless

Vertical loop (`flight_core/vertical_controller.hpp`). The output is the
collective as it really left, clamp included:

    vertical/setpoint  measurement  error             m/s
    vertical/p_term  i_term  output                   unitless

Mixer and state machine (`flight_core/flight_core.hpp`):

    mixer/motor_0  1  2  3                            unitless
    core/flight_phase [r]  core/pilot_mode [r]        unitless

drone_sim only (`platform_sim/truth_telemetry.hpp`), fed from the
composition root where the estimate and the plant state of the same instant
are both at hand:

    sim/truth/position_x  y  z                        m
    sim/truth/velocity_x  y  z                        m/s
    sim/truth/attitude/w  x  y  z                     unitless
    sim/attitude_error                                rad

Firmware only:

    sensor/baro_temperature                           degC (platform_stm32/bmp581.hpp)
    loop/step_duration                                us   (drone_firmware/firmware_app.hpp)

## Adding a measure

Declare a `TelemetryEntry` next to the value, add it to the list above, and
that is all: the provider picks it up from the registry at the next
`init()`, the gateway pulls the new table when the node comes up, and the
telemetry page offers it in its catalog. Nothing in the schema, the packer
or any codec changes - which is the whole reason the registry exists.

## The provider

`TelemetryProvider` (`telemetry/provider.hpp`) is the registry on the
wire, an `AbsMessageHandler` of the composition's `Messenger` declared last
among the services: `init()` freezes the registry into the table this node
publishes, so every object holding a measure must already exist. The id of
a measure is its index in that table, for the life of the process.

```cpp
TelemetryProvider telemetryProvider{messenger, MIN_TELEMETRY_PERIOD_MS};
telemetryProvider.init();            // last, once every measure exists
telemetryProvider.sample(frame.timestampUs);   // once per flight frame
```

Three messages reach it:

- `TelemetryListRequest { cursor }`, answered by one
  `TelemetryDescriptors { total, cursor, descriptors }` page; the consumer
  paces the walk, one page per request, and the last page is the one where
  `cursor + descriptors_count == total`.
- `TelemetryConfig { ids, period_ms }`: what the stream carries and how
  often, **one configuration per node, never one per consumer**. It
  replaces the enabled set and the period wholesale, last writer wins. The
  ids are filtered to the table and to `MAX_ENABLED`, kept ascending, and
  the period is clamped between the composition's floor and
  `MAX_PERIOD_MS` = 60 s. It is answered by the configuration as applied,
  to the node that asked and to every other subscriber: holding a stream
  means hearing what changes it. A period of 0, or no known id, stops the
  samples without touching the subscriptions.
- `TelemetrySubscribe { enabled }`: take the sample stream, or stop it.
  Answered by the same message as applied; `enabled` comes back false when
  the table is full (`MAX_SUBSCRIBERS` = 2). A node that goes down is
  dropped (`onNodeDown()`), so a tool that crashed leaves nothing streaming
  behind it.

`sample(nowUs)` emits one batch per subscriber when the period elapsed and
splits a sampling instant wider than `VALUES_PER_MESSAGE` into several
messages carrying the same timestamp. The samples go out with `send()`: a
sample is only worth its own instant. The provider never reads a clock, and
two time bases meet in it: the messenger is polled on the process clock
(the sim polls it from the sensor wait) while the stream is timed on the
frames, so a configuration is stamped with the instant of the last
`sample()`, never with the instant of the poll that delivered it.

### The period floor

`minPeriodMs` is a constructor argument because it describes the link, not
the loop:

- `drone_sim` passes 2 ms, the frame period. The plant paces the loop at
  500 Hz and a loopback datagram costs nothing, so there is nothing faster
  to ask for: a shorter period would only repeat a frame's values.
- the firmware passes 10 ms. At 921600 baud the serial framing carries about
  92 kB/s; 64 enabled measures are two `TelemetryData` messages of roughly
  300 bytes, so 100 Hz is about 60 kB/s. That leaves room for the Status
  stream, the log lines and the tuning answers sharing the same UART, and
  1 ms would not.

A period outside `[minPeriodMs, MAX_PERIOD_MS]` is clamped and the answer
says what was applied, so a ground tool never has to guess.
