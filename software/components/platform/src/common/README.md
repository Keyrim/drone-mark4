# software/components/platform/src/common

Composed helpers shared across variants (interface base classes stay pure -
shared code goes here, by composition): `RcTracker`, the RC decode and
fail-safe every composition grafts onto the frame it is about to step, and
`FrameTelemetry`, the copy of the last `SensorFrame` the platform measures
read out of. Both are IO adapters between the flight loop and something
else - the `Rc` message of `protocol/mark4.proto` on one side, the
telemetry registry on the other - so neither side ever sees the other.

What a node reports of itself is not here: `Status` and its packer live in
`software/components/status/`, the telemetry stream in
`software/components/telemetry/`, and each is a provider on the messenger
rather than a helper of the platform.

`FrameTelemetry` holds the copy of the last `SensorFrame` the platform
measures read from: every composition steps a frame it keeps as a local, so
the measures need an address of their own.
