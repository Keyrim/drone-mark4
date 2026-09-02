# telemetry

The registry of named float measures every node exposes. A leaf library
like `log`: static lib, `drone_warnings` alone, no heap, no iostream, no
exceptions, no RTTI, and it builds for the F405 as it stands. It knows
names, units and where values live; it knows nothing of the wire, of ids,
of periods or of subscribers. Those belong to the wire adapter
(`platform_common/telemetry_service.hpp`), which freezes the list into a
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
  wire adapter with a WARN, not truncated.
- `MAX_TELEMETRY_ENTRIES` = 128. The registry itself has no limit; this is
  the size of the frozen table the adapter indexes, and therefore the
  largest id that can travel. Entries past it are ignored with a WARN.
- Not thread-safe: every node registers and samples from its one loop
  thread.

## The measures

Registered today, by area. `[r]` marks a reader rather than a pointer.

TODO(tmagne): keep this list in step with the registrations.
