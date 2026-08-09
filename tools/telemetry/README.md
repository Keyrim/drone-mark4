# Serial telemetry tools

The board multiplexes two streams on the same UART link (FTDI dongle on
`/dev/ttyUSB0`, 921600 baud), each wrapped in the serial framing of
`protocol/serial_framing.hpp` (CRC-16 covering length + payload): 50 Hz
`TelemetryPacket` (`protocol/telemetry.hpp`) and full-rate blackbox
records (`protocol/blackbox.hpp`). Receivers demux on the version + type
header of each payload.

`read_serial.py` is the quick link check: it unpacks the telemetry
packets, counts the blackbox records, and prints a summary: packet
count, checksum failures, timestamp cadence and the latest state
snapshot.

```sh
python3 tools/telemetry/read_serial.py
```

Exit code 0 when more than 20 valid packets arrived within the capture
window. Standard library only, no pyserial needed.

During an actual bench session the `hub` is the single consumer of the
serial port, and nothing else may open it at the same time:

```sh
./build/desktop/apps/hub/hub up real --serial /dev/ttyUSB0
./build/desktop/apps/hub/hub serve --serial /dev/ttyUSB0 --record
```

The hub re-broadcasts the telemetry it decodes over UDP exactly like
drone_sim emits it (47801 plus the 47803 mirror), so the ground station
and the Godot attitude ghost work unchanged on real flights, and it
appends the blackbox records to a timestamped `.m4bb` file (raw record
sequence, the same format `drone_sim` writes) that `drone_replay` reads
back:

```sh
./build/desktop/apps/drone_replay/drone_replay logs/board_<stamp>.m4bb
```

The uplink goes through the hub too: an `rc` message on its websocket
endpoint (`{"type":"rc","target":"firmware",...}`) is framed onto the
UART verbatim. The Godot keyboard pilot (K = kill, A = arm, Up/Down =
throttle) remains the cockpit for a simulated flight, streaming
`RcCommandPacket` datagrams at 10 Hz to udp/47805 (`RC_COMMAND_PORT`),
where the local `drone_sim` binds its own command receiver. Both paths
fail-safe to kill+disarmed after 500 ms of silence, so closing the
sender is itself a safe action.

`read_serial.py` imports the wire constants from the shared
`tools/ground-station/telemetry_wire.py` module, the single python copy
of the protocol; the golden packet fixtures catch any drift in CI.
