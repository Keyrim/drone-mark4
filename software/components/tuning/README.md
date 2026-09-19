# tuning

The tunable parameters of the flight core on the wire. A header-only
target (`tuning_provider`) over `messaging` (the dispatch and the sender),
`flight_core` (the registry) and `log`; no heap, no iostream, no
exceptions, no RTTI, and it builds for the F405 as it stands. Its `src/`
holds one single-include source per header: the file that compiles the
header alone and that the editor's language server takes the header's
flags from.

## The provider

`TuningProvider` (`tuning/provider.hpp`) is an `AbsMessageHandler` of the
composition's `Messenger`, declared as a member after it. flight-core never
includes a wire header, so this is where the two vocabularies meet: the
`TuningStatus` values and the parameter name width are pinned to the wire
here, one `static_assert` each.

Three messages reach it, each answered to the node that asked:

- `TuningSet { id, value }` and `TuningGet { id }`, answered by a
  `TuningAck { id, value, status }`. The value that travels is always the
  live one, whatever the outcome: a refused write is answered with what is
  still flying, so a ground tool never has to guess what it ended up with.
- `TuningListRequest { cursor }`, answered by one `TuningInfos { total,
  cursor, infos }` page of at most `INFOS_PER_PAGE` descriptions. The
  consumer paces the walk, one page per request, and the last page is the
  one where `cursor + infos_count == total`. A cursor past the end comes
  back as an empty page carrying the total.

Every answer goes out with `request()`: an answer that matters is a
request of its own, acknowledged on arrival and resent until it is. The
page shape is what the UART wanted: a board that answered a list request by
dumping the whole table at once would flood a 921600 baud line for
milliseconds and starve the telemetry stream sharing it.

## Timing

`onMessage()` runs inside `Messenger::poll()`, which every composition runs
before `FlightCore::step()`: a value a request writes is in effect for the
whole of the next step and never changes one halfway through. The core is
single-threaded and so is this: no locking, no queue, and no clock read.

## The consumer

`tuning/consumer.hpp` holds the other side of the same concept, for a
ground node: `TuningConsumer<N>` (target `tuning_consumer`), an
`AbsMessageHandler` and an `AbsDirectoryListener` at once, sized by the
composition. Header-only, fixed tables, no heap.

```cpp
TuningConsumer<Transport::MAX_NODES> tuning{messenger, directory};  // after both
tuning.set(node, id, value);   // also get(), refresh()
```

The kinds that carry a `TuningProvider` are its own constant (`KINDS`:
`FIRMWARE`, `DRONE_SIM`). A node of one of those kinds whose announce
matches this wire hash is opened from `onIdentity()` and its table pulled
with a `TablePull<mark4_TuningInfo, MAX_PARAMS>`: one
`TuningListRequest` per page, the next asked for as each page lands, the
listeners told once the table is whole
(`AbsTuningConsumerListener::onTable()`). A `TuningAck` reaches
`onResult()`, and a write that went through moves the value in the table,
so what the consumer holds is what flies. A page request given up on
abandons the walk; a node that goes down loses its entry.
