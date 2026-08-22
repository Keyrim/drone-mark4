#!/usr/bin/env python3
"""Synthetic sensor stream over UDP, standing in for a physics simulator.

The stub sends sensor packets to the flight process (drone_sim listens on
127.0.0.1:47800 by default) and listens on the same socket for the actuator
packets the flight process sends back to the sender address. Simulated time
comes from the frame counter and the requested rate, so the stream content is
deterministic regardless of how the host schedules the process.

The wire layout comes from the shared telemetry_wire module, the single
Python copy of software/components/protocol/include/protocol/.

Python 3 standard library only.
"""

import argparse
import errno
import math
import os
import select
import socket
import sys
import time
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from telemetry_wire import (
    PROTOCOL_VERSION,
    SIM_ACTUATOR_PACKET_SIZE,
    SIM_ACTUATOR_STRUCT,
    SIM_LINK_PORT,
    SIM_SENSOR_STRUCT,
    TYPE_SIM_ACTUATOR,
    TYPE_SIM_SENSOR,
    has_header,
)

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = SIM_LINK_PORT
DEFAULT_RATE_HZ = 1000.0
DEFAULT_DURATION_S = 10.0
DEFAULT_AMPLITUDE_RAD_S = 0.5
DEFAULT_FREQUENCY_HZ = 2.0

GRAVITY_MPS2 = 9.80665
SEA_LEVEL_PRESSURE_PA = 101325.0
RESET_COUNT = 0
#: The stub is one plant that never restarts, and never times out waiting.
SESSION_ID = 0x5100B
LOCKSTEP_TIMEOUTS = 0

STATUS_PERIOD_S = 1.0
RECEIVE_BUFFER_SIZE = 512


def parse_arguments(argv: Optional[List[str]] = None) -> argparse.Namespace:
    """Parse the command line."""
    parser = argparse.ArgumentParser(
        description="Stream synthetic sensor packets to the flight process.",
    )
    parser.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help="destination address of the flight process (default: %(default)s)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help="destination UDP port (default: %(default)s)",
    )
    parser.add_argument(
        "--rate",
        type=float,
        default=DEFAULT_RATE_HZ,
        help="sensor frames per second (default: %(default)s)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=DEFAULT_DURATION_S,
        help="streaming duration in seconds, 0 runs until Ctrl-C "
        "(default: %(default)s)",
    )
    parser.add_argument(
        "--amplitude",
        type=float,
        default=DEFAULT_AMPLITUDE_RAD_S,
        help="gyro sine amplitude in rad/s (default: %(default)s)",
    )
    parser.add_argument(
        "--frequency",
        type=float,
        default=DEFAULT_FREQUENCY_HZ,
        help="gyro sine frequency in Hz (default: %(default)s)",
    )
    return parser.parse_args(argv)


def build_sensor_packet(
    frame_index: int, rate_hz: float, amplitude_rad_s: float, frequency_hz: float
) -> bytes:
    """Serialize the sensor frame for the given frame index."""
    simulated_time_s = frame_index / rate_hz
    timestamp_us = int(round(simulated_time_s * 1e6))
    phase_rad = 2.0 * math.pi * frequency_hz * simulated_time_s
    return SIM_SENSOR_STRUCT.pack(
        PROTOCOL_VERSION,
        TYPE_SIM_SENSOR,
        timestamp_us,
        amplitude_rad_s * math.sin(phase_rad),
        amplitude_rad_s * math.cos(phase_rad),
        0.0,
        0.0,
        0.0,
        GRAVITY_MPS2,
        SEA_LEVEL_PRESSURE_PA,
        RESET_COUNT,
        SESSION_ID,
        LOCKSTEP_TIMEOUTS,
    )


def decode_actuator_packet(payload: bytes) -> Optional[Tuple[float, ...]]:
    """Return the four motor commands, or None if the datagram is not ours.

    The echoed timestamp is ignored here: the stub free-runs. So is the
    scenario block trailing the motors: the stub has no world to reset.
    """
    if len(payload) != SIM_ACTUATOR_PACKET_SIZE:
        return None
    if not has_header(payload, TYPE_SIM_ACTUATOR):
        return None
    return SIM_ACTUATOR_STRUCT.unpack(payload)[3:7]


def drain_replies(
    sock: socket.socket, last_motors: Optional[Tuple[float, ...]]
) -> Tuple[int, Optional[Tuple[float, ...]]]:
    """Read every pending datagram without blocking.

    Returns the number of valid actuator packets and the last motor values
    seen (the previous value is kept when nothing valid arrived).
    """
    reply_count = 0
    while True:
        readable, _, _ = select.select([sock], [], [], 0.0)
        if not readable:
            return reply_count, last_motors
        try:
            payload, _ = sock.recvfrom(RECEIVE_BUFFER_SIZE)
        except OSError as error:
            # An unreachable peer surfaces here on some systems.
            if error.errno in (errno.ECONNREFUSED, errno.ECONNRESET):
                continue
            raise
        motors = decode_actuator_packet(payload)
        if motors is not None:
            reply_count += 1
            last_motors = motors


def send_packet(sock: socket.socket, packet: bytes, address: Tuple[str, int]) -> None:
    """Send one datagram, tolerating a missing receiver."""
    try:
        sock.sendto(packet, address)
    except OSError as error:
        # No listener on the far side: the kernel reports the ICMP port
        # unreachable of a previous datagram. Keep streaming anyway.
        if error.errno not in (errno.ECONNREFUSED, errno.ECONNRESET):
            raise


def format_motors(motors: Optional[Tuple[float, ...]]) -> str:
    """Render the last motor values for the status line."""
    if motors is None:
        return "none"
    return " ".join("{:.3f}".format(value) for value in motors)


def print_status(sent: int, received: int, motors: Optional[Tuple[float, ...]]) -> None:
    """Print one status line."""
    print(
        "sent {} replies {} motors [{}]".format(sent, received, format_motors(motors)),
        flush=True,
    )


def stream(arguments: argparse.Namespace) -> int:
    """Run the send and receive loop. Returns the process exit code."""
    if arguments.rate <= 0.0:
        print("error: --rate must be greater than zero", file=sys.stderr)
        return 2
    if arguments.duration < 0.0:
        print("error: --duration must not be negative", file=sys.stderr)
        return 2

    address = (arguments.host, arguments.port)
    period_s = 1.0 / arguments.rate
    total_frames = (
        0 if arguments.duration == 0.0 else int(arguments.duration * arguments.rate)
    )

    sent = 0
    received = 0
    last_motors: Optional[Tuple[float, ...]] = None

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    start_s = time.monotonic()
    try:
        print(
            "streaming to {}:{} at {:.1f} Hz ({})".format(
                arguments.host,
                arguments.port,
                arguments.rate,
                "until Ctrl-C" if total_frames == 0 else "{} frames".format(total_frames),
            ),
            flush=True,
        )
        next_deadline_s = start_s
        next_status_s = start_s + STATUS_PERIOD_S

        while total_frames == 0 or sent < total_frames:
            packet = build_sensor_packet(
                sent, arguments.rate, arguments.amplitude, arguments.frequency
            )
            send_packet(sock, packet, address)
            sent += 1

            new_replies, last_motors = drain_replies(sock, last_motors)
            received += new_replies

            now_s = time.monotonic()
            if now_s >= next_status_s:
                print_status(sent, received, last_motors)
                next_status_s += STATUS_PERIOD_S
                if next_status_s <= now_s:
                    next_status_s = now_s + STATUS_PERIOD_S

            next_deadline_s += period_s
            remaining_s = next_deadline_s - time.monotonic()
            if remaining_s > 0.0:
                time.sleep(remaining_s)
    except KeyboardInterrupt:
        print("", flush=True)
    finally:
        new_replies, last_motors = drain_replies(sock, last_motors)
        received += new_replies
        sock.close()

    elapsed_s = time.monotonic() - start_s
    effective_rate_hz = sent / elapsed_s if elapsed_s > 0.0 else 0.0
    print(
        "done: {} frames sent in {:.2f} s ({:.1f} Hz), {} replies, "
        "last motors [{}]".format(
            sent, elapsed_s, effective_rate_hz, received, format_motors(last_motors)
        ),
        flush=True,
    )
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    """Entry point."""
    return stream(parse_arguments(argv))


if __name__ == "__main__":
    sys.exit(main())
