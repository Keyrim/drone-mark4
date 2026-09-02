# software/components/platform/src/common

Composed helpers shared across variants (interface base classes stay pure -
shared code goes here, by composition): TelemetryPublisher (decimation) and
packTelemetry (state to the `Telemetry` message), RcTracker, TuningService,
OtaUpdater, and `sendEnvelope()` (envelope_io.hpp), the one place a message
meets the transport: it encodes on the stack and hands the bytes to a node
id, `BROADCAST_NODE` for everything a ground tool may want to watch. These
are the IO adapters between flight-core accessors and the wire messages of
`protocol/mark4.proto`, so neither side ever sees the other; the flight core
enums are pinned to the wire enums value by value here.
