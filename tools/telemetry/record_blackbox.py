"""Capture the blackbox stream from the board: sync on the serial framing,
demux blackbox records from telemetry packets by payload size, and append
the raw records to a .m4bb file that drone_replay can play back.

Usage: python3 tools/telemetry/record_blackbox.py [output.m4bb]
Stops on Ctrl-C; prints a one-line status every second."""

import os
import sys
import termios
import time

PORT = "/dev/ttyUSB0"
BAUD = termios.B921600
TELEMETRY_PACKET_SIZE = 95  # protocol/telemetry.hpp
BLACKBOX_RECORD_SIZE = 59  # flight_core/blackbox.hpp
BLACKBOX_VERSION = 2

if len(sys.argv) > 1:
    out_path = sys.argv[1]
else:
    os.makedirs("logs", exist_ok=True)
    out_path = time.strftime("logs/board_%Y%m%d_%H%M%S.m4bb")

fd = os.open(PORT, os.O_RDONLY | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = 0  # iflag: raw
attrs[1] = 0  # oflag
attrs[2] = termios.CREAD | termios.CLOCAL | termios.CS8 | BAUD  # cflag
attrs[3] = 0  # lflag: raw
attrs[4] = BAUD
attrs[5] = BAUD
attrs[6][termios.VMIN] = 0
attrs[6][termios.VTIME] = 5  # 0.5 s read timeout
termios.tcsetattr(fd, termios.TCSAFLUSH, attrs)

SYNC0, SYNC1 = 0xA5, 0x5A
state, length, payload = 0, 0, bytearray()
records, packets, bad = 0, 0, 0
next_status = time.time() + 1.0

out = open(out_path, "wb")
print(f"recording to {out_path}, Ctrl-C to stop")
try:
    while True:
        chunk = os.read(fd, 4096)
        now = time.time()
        if now >= next_status:
            next_status = now + 1.0
            print(f"\r{records} records, {packets} telemetry, {bad} bad ",
                  end="", flush=True)
        if not chunk:
            continue
        for byte in chunk:
            if state == 0:
                state = 1 if byte == SYNC0 else 0
            elif state == 1:
                state = 2 if byte == SYNC1 else (1 if byte == SYNC0 else 0)
            elif state == 2:
                length, payload, state = byte, bytearray(), 3
            elif state == 3:
                payload.append(byte)
                if len(payload) == length:
                    state = 4
            else:
                checksum = 0
                for b in payload:
                    checksum ^= b
                if checksum != byte:
                    bad += 1
                elif (length == BLACKBOX_RECORD_SIZE
                      and payload[0] == BLACKBOX_VERSION):
                    out.write(payload)
                    records += 1
                elif length == TELEMETRY_PACKET_SIZE:
                    packets += 1
                else:
                    bad += 1
                state = 0
except KeyboardInterrupt:
    pass
finally:
    out.close()
    os.close(fd)

print(f"\n{out_path}: {records} records "
      f"({records * BLACKBOX_RECORD_SIZE} bytes), "
      f"{packets} telemetry packets, {bad} bad frames")
sys.exit(0 if records > 0 else 1)
