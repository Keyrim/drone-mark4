"""Quick check of the UART link: sync on the serial framing, unpack
TelemetryPacket (protocol/telemetry.hpp), count the interleaved blackbox
records, and print a summary."""

import os
import sys
import termios
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ground-station"))
from telemetry_wire import (
    BLACKBOX_RECORD_SIZE,
    CRC16_INIT,
    SERIAL_SYNC0,
    SERIAL_SYNC1,
    TELEMETRY_PACKET_SIZE,
    TELEMETRY_STRUCT,
    TYPE_TELEMETRY,
    crc16,
    has_header,
)

PORT = "/dev/ttyUSB0"
BAUD = termios.B921600

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

SYNC0, SYNC1 = SERIAL_SYNC0, SERIAL_SYNC1
state, length, payload, crc_low = 0, 0, bytearray(), 0
packets, records, bad, last_ts = [], 0, 0, None
deadline = time.time() + 4.0

while time.time() < deadline and len(packets) < 60:
    chunk = os.read(fd, 4096)
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
        elif state == 4:
            crc_low, state = byte, 5
        else:
            crc = crc16(CRC16_INIT, bytes([length]) + payload)
            if crc != (byte << 8 | crc_low):
                bad += 1
            elif (length == TELEMETRY_PACKET_SIZE
                  and has_header(payload, TYPE_TELEMETRY)):
                packets.append(TELEMETRY_STRUCT.unpack(bytes(payload)))
            elif length == BLACKBOX_RECORD_SIZE:
                records += 1
            else:
                bad += 1
            state = 0
os.close(fd)

print(f"packets: {len(packets)} valid, {records} blackbox records, {bad} bad")
if packets:
    deltas = [
        (b[4] - a[4]) for a, b in zip(packets, packets[1:])
    ]
    print(f"timestamp delta: min {min(deltas)} max {max(deltas)} us")
    p = packets[-1]
    print(f"version {p[0]}  source {p[2]}  seq {p[3]}  t {p[4]} us")
    print(f"gyro {p[5]:+.4f} {p[6]:+.4f} {p[7]:+.4f} rad/s")
    print(f"quat w {p[8]:+.4f}  alt {p[19]:+.3f} m  vz {p[20]:+.3f} m/s")
    print(f"motors {p[15]:.2f} {p[16]:.2f} {p[17]:.2f} {p[18]:.2f}  "
          f"throwState {p[21]} throws {p[22]} phase {p[26]}")
sys.exit(0 if len(packets) > 20 else 1)
