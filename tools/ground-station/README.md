# Ground station

Real-time viewer for the drone UDP broadcasts. Four stacked plots:

- body angular rates, from the telemetry broadcast of the flight process;
- attitude as Euler angles, estimated (telemetry, solid) against the exact
  simulator state (sim raw broadcast, dashed);
- attitude error angle between the two quaternions, the estimator's
  validation metric;
- altitude and vertical velocity, estimated against exact.

Without a simulator the sim raw stream stays silent and only the estimated
curves draw. Packet decoding (`telemetry_wire.py`) is kept separate from the
GUI (`ground_station.py`) and can be tested headless.

## Install

Dependencies are managed with pipenv (`Pipfile` / `Pipfile.lock`). The
devcontainer image ships pipenv and sets `PIPENV_VENV_IN_PROJECT=1`, so the
virtual environment lands in `./.venv/` (gitignored), where the VS Code debug
configuration expects it:

```sh
cd tools/ground-station
pipenv install
```

pipenv is only a convenience; any environment providing pyqtgraph and PyQt6
works.

## Run

```sh
pipenv run ./ground_station.py            # ports 47801 + 47802, 10 s of history
pipenv run ./ground_station.py --window 30
```

Or press F5 in VS Code with the `ground_station (python)` launch
configuration.

Options:

- `--telemetry-port`: UDP port of the telemetry broadcast, default 47801.
- `--sim-raw-port`: UDP port of the raw simulator broadcast, default 47802.
- `--window`: seconds of history shown on the plot, default 10.

The socket is bound to `0.0.0.0` with `SO_REUSEADDR`, so several tools can
listen to the same broadcast at the same time. Datagrams with an unexpected
size or an unknown protocol version are dropped; the window title shows how
many packets were received and how many were dropped.

## Packet format

The wire format is defined by the firmware headers, which are the source of
truth:

- `protocol/include/protocol/telemetry.hpp`: `TelemetryPacket` layout, packed
  size and UDP port.
- `protocol/include/protocol/sim_raw.hpp`: `SimRawPacket` layout, the exact
  simulator state.
- `protocol/include/protocol/version.hpp`: protocol version byte.

`telemetry_wire.py` mirrors those layouts and asserts the packed sizes at
import time. Update it whenever the headers change.
