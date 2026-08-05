#!/usr/bin/env python3
"""Record the telemetry and sim raw UDP broadcasts to CSV files.

Waits for the first packet, then records until the streams go idle (or until
--duration seconds of data). Writes one timestamped CSV pair into logs/, ready
for stream_compare.py. Python stdlib only.
"""

import argparse
import csv
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ground-station"))
from telemetry_wire import (
    SIM_RAW_PORT,
    TELEMETRY_PORT,
    decode_sim_raw,
    decode_telemetry,
)

IDLE_STOP_S = 5.0
WAIT_LOG_PERIOD_S = 10.0

TELEMETRY_HEADER = (
    "timestamp_us,gyro_x,gyro_y,gyro_z,quat_w,quat_x,quat_y,quat_z,"
    "bias_x,bias_y,bias_z,motor_0,motor_1,motor_2,motor_3,altitude_m,vz_mps"
)
SIM_RAW_HEADER = "timestamp_us,quat_w,quat_x,quat_y,quat_z,pos_x,pos_y,pos_z,vel_x,vel_y,vel_z"


def open_socket(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.setblocking(False)
    return sock


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--duration", type=float, default=0.0,
        help="max seconds of data after the first packet (0 = until idle)",
    )
    args = parser.parse_args()

    telemetry_sock = open_socket(TELEMETRY_PORT)
    sim_raw_sock = open_socket(SIM_RAW_PORT)

    stamp = time.strftime("%Y%m%d_%H%M%S")
    telemetry_path = f"logs/streams_{stamp}_telemetry.csv"
    sim_raw_path = f"logs/streams_{stamp}_simraw.csv"
    os.makedirs("logs", exist_ok=True)

    counts = {"telemetry": 0, "simraw": 0}
    started = None
    last_packet = None
    print(f"recording to {telemetry_path} + {sim_raw_path}")
    print("waiting for packets (start the simulator and the flight process)...")

    with open(telemetry_path, "w", newline="") as tel_file, \
         open(sim_raw_path, "w", newline="") as raw_file:
        tel_csv = csv.writer(tel_file)
        raw_csv = csv.writer(raw_file)
        tel_csv.writerow(TELEMETRY_HEADER.split(","))
        raw_csv.writerow(SIM_RAW_HEADER.split(","))

        last_wait_log = time.monotonic()
        while True:
            now = time.monotonic()
            if started is not None:
                if now - last_packet > IDLE_STOP_S:
                    print("streams idle, stopping")
                    break
                if args.duration > 0.0 and now - started > args.duration:
                    print("requested duration reached, stopping")
                    break
            elif now - last_wait_log > WAIT_LOG_PERIOD_S:
                print("still waiting...")
                last_wait_log = now

            readable, _, _ = select.select([telemetry_sock, sim_raw_sock], [], [], 0.5)
            for sock in readable:
                datagram = sock.recv(2048)
                if sock is telemetry_sock:
                    sample = decode_telemetry(datagram)
                    if sample is None:
                        continue
                    tel_csv.writerow(
                        (sample.timestamp_us, *sample.gyro_rad_s, *sample.attitude_quat,
                         *sample.gyro_bias_rad_s, *sample.motor,
                         sample.altitude_m, sample.vertical_velocity_mps)
                    )
                    counts["telemetry"] += 1
                else:
                    sample = decode_sim_raw(datagram)
                    if sample is None:
                        continue
                    raw_csv.writerow(
                        (sample.timestamp_us, *sample.attitude_quat,
                         *sample.position_m, *sample.velocity_mps)
                    )
                    counts["simraw"] += 1
                if started is None:
                    started = time.monotonic()
                    print("first packet, recording")
                last_packet = time.monotonic()

    print(f"done: {counts['telemetry']} telemetry rows, {counts['simraw']} sim raw rows")
    return 0 if counts["telemetry"] or counts["simraw"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted")
