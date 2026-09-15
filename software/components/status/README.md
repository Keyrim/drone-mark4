# status

What a drone is doing, as the wire says it. A header-only INTERFACE target
(`status_provider`) over `messaging` (the dispatch and the sender),
`flight_core` (the state read) and `log`; no heap, no iostream, no
exceptions, no RTTI, and it builds for the F405 as it stands.

## What Status is

`Status` is the small fixed report of what the drone is doing, not a
measurement stream. Fixed content: attitude, motors, phase, throw state and
count, the two validity flags, the RC link flag, and the plant truth when
the composition has one. It answers "where is it and is it healthy", which
is what a control page and an attitude view need on every frame they paint.

A quantity that belongs to one module rather than to every consumer is
registered as a telemetry measure instead
(`software/components/telemetry/README.md`): the registry is where a new
number goes, and a field is added to `Status` only when every consumer of
`Status` needs it.

## The packer

`status/status_packer.hpp` holds `packStatus()`, an IO adapter that reads
only public accessors of the flight core, so flight-core never sees a wire
type. It is also where the flight core enums and their wire counterparts
are pinned to each other, value by value.

## The provider

`status/status_provider.hpp` holds `StatusProvider`, an
`AbsMessageHandler` of the composition's `Messenger`:

```cpp
StatusProvider statusProvider{messenger};          // a member after the messenger
statusProvider.publish(frame, actuators, core, rcLinkOk, truth);  // once per flight frame
```

It consumes `StatusSubscribe`. A node that subscribes is added to the
table and answered with the subscription as applied; `enabled` comes back
false when the table is full or when the request was an unsubscribe. A node
that goes down is dropped (`onNodeDown()`), so a consumer that crashed
leaves nothing streaming behind it.

`publish()` counts the frames and packs one report every
`STATUS_PERIOD_FRAMES` = 10, so a 500 Hz loop reports at 50 Hz. The
provider owns that counter, so every composition decimates the same stream
the same way. With no subscriber nothing is packed at all. The reports go
out with `send()`: a report is only worth its own instant, and the next one
is ten frames away.

`MAX_SUBSCRIBERS` = 4. It is small on purpose: one emission per entry, and
a board's UART pays every one of them at 50 Hz, next to the log lines and
the telemetry stream sharing the same line. The provider never reads a
clock: `publish()` takes the frame's own timestamp through the frame.
