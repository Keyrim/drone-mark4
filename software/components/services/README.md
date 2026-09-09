# services

The wire services of a flight composition: what answers a request on the
transport when the node is a drone (`drone_sim`, `drone_firmware`). A
header-only INTERFACE target over `messaging` (the dispatch and the
sender), `flight_core` (the parameter table), `telemetry` (the registry),
`log` and `ota` (the updater); no heap, no iostream, no exceptions, no
RTTI, and it builds for the F405 as it stands.

## The model

Each service is one `AbsMessageHandler` of the composition's `Messenger`:
it names the body tags it consumes in a `static constexpr TAGS` array,
attaches in its constructor and detaches in its destructor, so declaring
it as a member after the messenger is the whole wiring. Each one answers
the node the request came from (`src`), never the whole bench: a request
is a conversation with one node. What every node must hear (a `Status`,
a `Log` line, the sim's run stats) is not a service's business and leaves
through the transport as a broadcast.

| header | class | consumes | answers |
|--------|-------|----------|---------|
| `services/telemetry_service.hpp` | `TelemetryService` | `TelemetryListRequest`, `TelemetryEnable` | `TelemetryDescriptors` (one page per request), `TelemetryAck`, then `TelemetryData` to the one subscriber at the period it asked for |
| `services/tuning_service.hpp` | `TuningService` | `TuningSet`, `TuningGet`, `TuningList` | `TuningAck` with the value in effect; `TuningInfo`, one per `pump()`, to the node that asked for the list |
| `services/ota_service.hpp` | `OtaService` | `OtaStatusRequest`, `OtaBegin`, `OtaChunk`, `OtaFinish`, `OtaRevert`, `OtaAbort` | whatever the `OtaUpdater` replies (`OtaStatus`, `OtaAck`, `OtaChunkAck`), at most one per request |

## Timing

`onMessage()` runs inside `Messenger::poll()`, which every composition runs
before `FlightCore::step()`: a value a request writes is in effect for the
whole of the next step and never changes one halfway through.

Two of the services also have a per-frame method, called after the step
with the frame's own timestamp: `TelemetryService::sample(nowUs)` emits a
batch when the period elapsed and stops the stream once the subscriber
went silent, `TuningService::pump()` emits one parameter description of a
pending list. Neither service reads a clock.

The telemetry stream is timed on the frames, while the poll that delivers
an enable may run on another clock (the sim polls its messenger from the
sensor wait, on the process clock): `TelemetryService` stamps an enable
with the timestamp of the last `sample()`, never with the poll's instant.

`OtaService::consumed()` counts the requests the updater consumed. A
composition that caches something the updater may change (the arming
interlock read off the boot metadata) compares it once per frame and
re-reads when it moved, instead of re-reading on every frame.
