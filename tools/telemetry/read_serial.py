"""Quick check of the UART telemetry: sync on the serial framing, unpack
TelemetryPacket (protocol/telemetry.hpp) and print a summary."""

import os
import struct
import sys
import termios
import time

PORT = "/dev/ttyUSB0"
BAUD = termios.B115200
# version u8, timestampUs u64, gyro 3f, quat 4f, bias 3f, motor 4f,
# altitude f, vz f, throwState u8, throwCount u32, releaseVel f,
# apexTimestampUs u64, apexAlt f, flightPhase u8  -> packed little-endian
FMT = "<BQ3f4f3f4fffBIfQfB"
SIZE = struct.calcsize(FMT)
assert SIZE == 95, SIZE

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
packets, bad, last_ts = [], 0, None
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
        else:
            checksum = 0
            for b in payload:
                checksum ^= b
            if checksum == byte and length == SIZE:
                packets.append(struct.unpack(FMT, bytes(payload)))
            else:
                bad += 1
            state = 0
os.close(fd)

print(f"packets: {len(packets)} valid, {bad} bad")
if packets:
    deltas = [
        (b[1] - a[1]) for a, b in zip(packets, packets[1:])
    ]
    print(f"timestamp delta: min {min(deltas)} max {max(deltas)} us")
    p = packets[-1]
    print(f"version {p[0]}  t {p[1]} us")
    print(f"gyro {p[2]:+.4f} {p[3]:+.4f} {p[4]:+.4f} rad/s")
    print(f"quat w {p[5]:+.4f}  alt {p[16]:+.3f} m  vz {p[17]:+.3f} m/s")
    print(f"motors {p[12]:.2f} {p[13]:.2f} {p[14]:.2f} {p[15]:.2f}  "
          f"throwState {p[18]} throws {p[19]} phase {p[23]}")
sys.exit(0 if len(packets) > 20 else 1)
