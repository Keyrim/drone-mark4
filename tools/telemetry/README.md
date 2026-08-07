# Serial telemetry tools

The board multiplexes two streams on the same UART link (FTDI dongle on
`/dev/ttyUSB0`, 921600 baud), each wrapped in the serial framing of
`protocol/serial_framing.hpp`: 50 Hz `TelemetryPacket` (95 bytes,
`protocol/telemetry.hpp`) and full-rate blackbox records (59 bytes,
`flight_core/blackbox.hpp`). Receivers demux by payload size.

`read_serial.py` is the quick link check: it unpacks the telemetry
packets, counts the blackbox records, and prints a summary: packet
count, checksum failures, timestamp cadence and the latest state
snapshot.

```sh
python3 tools/telemetry/read_serial.py
```

Exit code 0 when more than 20 valid packets arrived within the capture
window. Standard library only, no pyserial needed.

`record_blackbox.py` captures the blackbox stream to a `.m4bb` file
(raw record sequence, the same format `drone_sim` writes) until Ctrl-C:

```sh
python3 tools/telemetry/record_blackbox.py [logs/flight.m4bb]
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb
```

The struct format string and the two payload sizes must be kept in sync
with the headers; the assert on the packed size (95) catches a drift at
startup.
