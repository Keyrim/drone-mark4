# Serial telemetry reader

`read_serial.py` reads the UART telemetry stream (FTDI dongle on
`/dev/ttyUSB0`, 115200 baud), synchronizes on the serial framing
(`protocol/serial_framing.hpp`), unpacks `TelemetryPacket`
(`protocol/telemetry.hpp`) and prints a summary: packet count, checksum
failures, timestamp cadence and the latest state snapshot.

```sh
python3 tools/telemetry/read_serial.py
```

Exit code 0 when more than 20 valid packets arrived within the capture
window. Standard library only, no pyserial needed.

The struct format string must be kept in sync with
`protocol/telemetry.hpp`; the assert on the packed size (95) catches a
drift at startup.
