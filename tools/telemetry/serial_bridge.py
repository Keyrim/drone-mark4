"""Bridge the board's UART to the rest of the tooling, playing the role
the ESP32 bridge will hold: the single owner of the serial port, keeping
every other tool on the UDP boundary of protocol/.

Downlink: TelemetryPacket (identified by its version + type header) is
re-broadcast over UDP exactly like drone_sim emits it (broadcast on 47801
plus the 47803 mirror for Godot), so the ground station and the simulator
ghost view work unchanged on real flights; blackbox records
(self-framing protocol/ records, checked sync to CRC) are appended to a
.m4bb file that drone_replay plays back.

Uplink: SimCommandPacket RC datagrams received on udp/47805 (the same
packet the Godot simulator consumes on 47804, on a port of its own
because the sim binds exclusively and the ghost view runs both at once)
are translated to RcCommandPacket on the serial line, one for one: the
pilot tool streams, and its silence propagates to the board fail-safe.

Usage: python3 tools/telemetry/serial_bridge.py [output.m4bb]
Stops on Ctrl-C; prints a one-line status every second."""

import os
import socket
import sys
import termios
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ground-station"))
from telemetry_wire import (
    BOARD_REBOOT_MAGIC,
    CRC16_INIT,
    SERIAL_SYNC0,
    SERIAL_SYNC1,
    PROTOCOL_VERSION,
    RC_COMMAND_PORT,
    RC_COMMAND_STRUCT,
    REBOOT_COMMAND_STRUCT,
    SIM_COMMAND_RC,
    SIM_COMMAND_RESET,
    SIM_COMMAND_STRUCT,
    TELEMETRY_MIRROR_PORT,
    TELEMETRY_PACKET_SIZE,
    TELEMETRY_PORT,
    BLACKBOX_RECORD_SIZE,
    TYPE_RC_COMMAND,
    TYPE_REBOOT_COMMAND,
    TYPE_SIM_COMMAND,
    TYPE_TELEMETRY,
    crc16,
    encode_serial_frame,
    has_header,
    valid_blackbox_record,
)

PORT = "/dev/ttyUSB0"
BAUD = termios.B921600

if len(sys.argv) > 1:
    out_path = sys.argv[1]
else:
    os.makedirs("logs", exist_ok=True)
    out_path = time.strftime("logs/board_%Y%m%d_%H%M%S.m4bb")

fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY)
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

udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

commands = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
commands.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
commands.bind(("", RC_COMMAND_PORT))
commands.setblocking(False)

SYNC0, SYNC1 = SERIAL_SYNC0, SERIAL_SYNC1
state, length, payload, crc_low = 0, 0, bytearray(), 0
records, packets, bad, rc_sent = 0, 0, 0, 0
next_status = time.time() + 1.0

out = open(out_path, "wb")
print(f"bridging {PORT}: telemetry -> udp/{TELEMETRY_PORT}+{TELEMETRY_MIRROR_PORT}, "
      f"blackbox -> {out_path}, rc <- udp/{RC_COMMAND_PORT}, Ctrl-C to stop")
try:
    while True:
        # Uplink first: translate pending RC commands, one for one.
        while True:
            try:
                datagram = commands.recv(256)
            except BlockingIOError:
                break
            if len(datagram) != SIM_COMMAND_STRUCT.size:
                continue
            if not has_header(datagram, TYPE_SIM_COMMAND):
                continue
            fields = SIM_COMMAND_STRUCT.unpack(datagram)
            command, kill, arm, mode, throttle = fields[2:7]
            if command == SIM_COMMAND_RC:
                up_payload = RC_COMMAND_STRUCT.pack(
                    PROTOCOL_VERSION, TYPE_RC_COMMAND, kill, arm, mode, throttle)
            elif command == SIM_COMMAND_RESET:
                up_payload = REBOOT_COMMAND_STRUCT.pack(
                    PROTOCOL_VERSION, TYPE_REBOOT_COMMAND, BOARD_REBOOT_MAGIC)
            else:
                continue
            os.write(fd, encode_serial_frame(up_payload))
            rc_sent += 1

        chunk = os.read(fd, 4096)
        now = time.time()
        if now >= next_status:
            next_status = now + 1.0
            print(f"\r{records} records, {packets} telemetry, {bad} bad, "
                  f"{rc_sent} rc up ", end="", flush=True)
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
                elif (length == BLACKBOX_RECORD_SIZE
                      and valid_blackbox_record(bytes(payload))):
                    out.write(payload)
                    records += 1
                elif (length == TELEMETRY_PACKET_SIZE
                      and has_header(payload, TYPE_TELEMETRY)):
                    datagram = bytes(payload)
                    udp.sendto(datagram, ("255.255.255.255", TELEMETRY_PORT))
                    udp.sendto(datagram,
                               ("255.255.255.255", TELEMETRY_MIRROR_PORT))
                    packets += 1
                else:
                    bad += 1
                state = 0
except KeyboardInterrupt:
    pass
finally:
    out.close()
    udp.close()
    commands.close()
    os.close(fd)

print(f"\n{out_path}: {records} records "
      f"({records * BLACKBOX_RECORD_SIZE} bytes), "
      f"{packets} telemetry packets forwarded, {rc_sent} rc commands up, "
      f"{bad} bad frames")
sys.exit(0 if records > 0 else 1)
