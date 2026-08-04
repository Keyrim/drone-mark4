# Ground station

Real-time telemetry viewer. It listens to the UDP telemetry broadcast emitted
by the flight process and plots the body angular rates as they arrive. This is
the first panel of a larger tool: attitude view, motor outputs and command
sending will come later, which is why packet decoding (`telemetry_wire.py`)
is kept separate from the GUI (`ground_station.py`) and can be tested
headless.

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
pipenv run ./ground_station.py            # port 47801, 10 s of history
pipenv run ./ground_station.py --port 47801 --window 30
```

Or press F5 in VS Code with the `ground_station (python)` launch
configuration.

Options:

- `--port`: UDP port to listen on, default 47801.
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
- `protocol/include/protocol/version.hpp`: protocol version byte.

`telemetry_wire.py` mirrors that layout (`struct` format `<BQ3f4f`, 37 bytes)
and asserts the packed size at import time. Update it whenever the headers
change.
