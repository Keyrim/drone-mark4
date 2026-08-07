"""Bridge the board's UART to the rest of the tooling, playing the role
the ESP32 bridge will hold: the single owner of the serial port, keeping
every other tool on the UDP boundary of protocol/.

Downlink, demuxed by payload size: TelemetryPacket (95 bytes) is
re-broadcast over UDP exactly like drone_sim emits it (broadcast on 47801
plus the 47803 mirror for Godot), so the ground station and the simulator
ghost view work unchanged on real flights; blackbox records (59 bytes)
are appended to a .m4bb file that drone_replay plays back.

Uplink: SimCommandPacket RC datagrams received on udp/47805 (the same
packet the Godot simulator consumes on 47804, on a port of its own
because the sim binds exclusively and the ghost view runs both at once)
are translated to RcCommandPacket on the serial line, one for one: the
pilot tool streams, and its silence propagates to the board fail-safe.

Usage: python3 tools/telemetry/serial_bridge.py [output.m4bb]
Stops on Ctrl-C; prints a one-line status every second."""

import os
import socket
import struct
import sys
import termios
import time

PORT = "/dev/ttyUSB0"
BAUD = termios.B921600
PROTOCOL_VERSION = 9  # protocol/version.hpp
TELEMETRY_PACKET_SIZE = 95  # protocol/telemetry.hpp
TELEMETRY_PORT = 47801
TELEMETRY_MIRROR_PORT = 47803
BLACKBOX_RECORD_SIZE = 59  # flight_core/blackbox.hpp
BLACKBOX_VERSION = 2
RC_COMMAND_PORT = 47805  # protocol/commands.hpp
SIM_COMMAND_RESET = 1
SIM_COMMAND_RC = 2
COMMAND_STRUCT = struct.Struct("<BBBBf3f3f4f")  # SimCommandPacket
BOARD_REBOOT_MAGIC = 0xB7  # RebootCommandPacket

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

SYNC0, SYNC1 = 0xA5, 0x5A
state, length, payload = 0, 0, bytearray()
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
            if len(datagram) != COMMAND_STRUCT.size:
                continue
            fields = COMMAND_STRUCT.unpack(datagram)
            version, command, kill, arm, throttle = fields[:5]
            if version != PROTOCOL_VERSION:
                continue
            if command == SIM_COMMAND_RC:
                up_payload = struct.pack("<BBBf", PROTOCOL_VERSION, kill, arm, throttle)
            elif command == SIM_COMMAND_RESET:
                up_payload = struct.pack("<BB", PROTOCOL_VERSION, BOARD_REBOOT_MAGIC)
            else:
                continue
            checksum = 0
            for b in up_payload:
                checksum ^= b
            os.write(fd, bytes([SYNC0, SYNC1, len(up_payload)]) + up_payload
                     + bytes([checksum]))
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
