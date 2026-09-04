# software/components/platform/src/common

Composed helpers shared across variants (interface base classes stay pure -
shared code goes here, by composition): StatusPublisher (decimation) and
packStatus (state to the `Status` message), TelemetryService, FrameTelemetry,
RcTracker, TuningService, and `sendEnvelope()` (envelope_io.hpp),
the one place a message meets the transport: it encodes on the stack and
hands the bytes to a node id, `BROADCAST_NODE` for everything a ground tool
may want to watch. These are the IO adapters between flight-core accessors
and the wire messages of `protocol/mark4.proto`, so neither side ever sees
the other; the flight core enums are pinned to the wire enums value by value
here.

## Status versus telemetry

Two different things, on purpose.

`StatusPublisher` broadcasts one `Status` every `DECIMATION` = 10 frames, so
a 500 Hz loop reports at 50 Hz, always, to nobody in particular. Fixed
content: attitude, motors, phase, throw state and count, the two validity
flags, the plant truth when the composition has one. It answers "where is it
and is it healthy", which is what a control page, an attitude view and a
campaign verdict need on every frame they paint.

`TelemetryService` is the wire adapter of the registry
(`software/components/telemetry/README.md`). It freezes the registry into a
table in `init()` - the id of a measure is its index in it, for the life of
the process - answers `TelemetryListRequest` with one page, applies
`TelemetryEnable` (which also keeps the stream alive) and streams
`TelemetryData` batches to the one node that asked. Unicast throughout, one
active stream per drone, last writer wins.

The two never read a clock: `publish()` and `sample()` take the frame's own
timestamp, and `sample()` runs once per flight frame right after `step()`
and the motor push.

### The period floor

`minPeriodMs` is a constructor argument because it describes the link, not
the loop:

- `drone_sim` passes 2 ms, the frame period. The plant paces the loop at
  500 Hz and a loopback datagram costs nothing, so there is nothing faster
  to ask for: a shorter period would only repeat a frame's values.
- the firmware passes 10 ms. At 921600 baud the serial framing carries about
  92 kB/s; 64 enabled measures are two `TelemetryData` messages of roughly
  300 bytes, so 100 Hz is about 60 kB/s. That leaves room for the Status
  stream, the log lines and the tuning answers sharing the same UART, and 1 ms
  would not.

A period outside `[minPeriodMs, MAX_PERIOD_MS]` is clamped and the
`TelemetryAck` says what was applied, so a ground tool never has to guess.
`FrameTelemetry` holds the copy of the last `SensorFrame` the platform
measures read from: every composition steps a frame it keeps as a local, so
the measures need an address of their own.
