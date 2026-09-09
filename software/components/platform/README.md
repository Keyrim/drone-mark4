# platform

The three abstract services the flight loop is composed from
(`include/platform/`): `AbsSensorSource`, `AbsMotorSink`, `AbsClock`. `AbsSensorSource::waitFrame()` is the
single wait point of the whole system; `AbsClock` serves the platform
services between themselves and is never handed to the flight core, which
reads the time off the frame.

Commands do not come in through a platform service: every message
addressed to the node is dispatched by the composition's `Messenger`
(`software/components/messaging/`) to the handler of its body tag, and the
request/answer services live in `software/components/services/`.

There is no output service: what a composition reports unasked (the
`Status`, the sim's run stats, the log lines) leaves through the
`Transport` it already holds as a broadcast (`sendEnvelope()` in
`src/common/include/platform_common/envelope_io.hpp`); what it answers
goes back to the requester through the messenger.

Implementations live under `src/<variant>/` (`sim`, `stm32`, each with its
own README) and the helpers shared by every variant under `src/common/`:
the status packer and publisher, the RC tracker, the frame measures.

## Frame validity contract

Every `SensorFrame` a source delivers carries two flags, `imuValid` and
`baroValid`. **Valid means a fresh measurement acquired for this frame.**
A source that could not read the sensor delivers the frame anyway, at the
nominal cadence, with the flag false and the field zero: it never replays an
old sample under a new timestamp, and it never withholds the frame. The
frame itself is what keeps the loop alive (RC, commands, telemetry); the
flags are what keep the flight core honest about what it may integrate (see
`software/components/flight-core/README.md`). The timestamp is always
genuine acquisition time from the variant's clock, sensors or not.
