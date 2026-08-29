# software/components/platform/src/common

Composed helpers shared across variants (interface base classes stay pure -
shared code goes here, by composition): TelemetryPublisher (decimation and
stream identity) and packTelemetry (state to wire). These are the IO
adapters between flight-core accessors and protocol/ layouts, so neither
side ever sees the other.
