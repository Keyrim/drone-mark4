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

`serial_bridge.py` is the single consumer of the serial port during a
session: telemetry packets are re-broadcast over UDP exactly like
drone_sim emits them (47801 plus the 47803 mirror), so the ground
station and the Godot attitude ghost work unchanged on real flights,
and blackbox records are appended to a `.m4bb` file (raw record
sequence, the same format `drone_sim` writes) until Ctrl-C:

```sh
python3 tools/telemetry/serial_bridge.py [logs/flight.m4bb]
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb
```

The uplink has no dedicated tool: the Godot simulator is the cockpit.
Its keyboard pilot (K = kill, A = arm, Up/Down = throttle) streams
`SimCommandPacket` RC datagrams at 10 Hz to udp/47805
(`RC_COMMAND_PORT`, distinct from the sim's own 47804 which Godot
binds exclusively), and the bridge relays them to the board as
`RcCommandPacket` over the UART. The firmware fail-safes to
kill+disarmed after 500 ms of silence, so closing either the bridge
or the simulator is a safe action.

Both tools import the wire constants from the shared
`tools/ground-station/telemetry_wire.py` module, the single python copy
of the protocol; the golden packet fixtures catch any drift in CI.
