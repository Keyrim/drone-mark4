#!/usr/bin/env python3
"""Decode a .m4bb blackbox file.

Prints a human summary by default, or every record as CSV with --csv.
Without a file argument, picks the newest .m4bb in logs/.
"""

import argparse
import math
import os
import pathlib
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from telemetry_wire import BLACKBOX_RECORD_SIZE, iter_blackbox_records

CSV_HEADER = (
    "timestamp_us,gyro_x_rad_s,gyro_y_rad_s,gyro_z_rad_s,"
    "accel_x_mps2,accel_y_mps2,accel_z_mps2,baro_pa,kill_switch,throttle,arm_switch,"
    "motor_0,motor_1,motor_2,motor_3"
)
G_MPS2 = 9.80665

# Field indices in the unpacked record tuple: sync (2), version, type,
# length, then the payload.
FIELD_TIMESTAMP = 5
FIELD_GYRO = 6
FIELD_ACCEL = 9
FIELD_KILL = 13
FIELD_CRC = 20


def newest_log() -> pathlib.Path:
    logs = sorted(pathlib.Path("logs").glob("*.m4bb"), key=lambda p: p.stat().st_mtime)
    if not logs:
        sys.exit("no .m4bb file in logs/")
    return logs[-1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", nargs="?", help="log file (default: newest in logs/)")
    parser.add_argument("--csv", action="store_true", help="dump every record as CSV")
    args = parser.parse_args()

    path = pathlib.Path(args.file) if args.file else newest_log()
    data = path.read_bytes()
    records = list(iter_blackbox_records(data))
    if not records:
        sys.exit(f"{path}: no valid record")
    damaged = len(data) - len(records) * BLACKBOX_RECORD_SIZE
    if damaged:
        print(f"warning: {damaged} bytes skipped (torn or foreign records)",
              file=sys.stderr)

    if args.csv:
        print(CSV_HEADER)
        for r in records:
            print(",".join(repr(v) for v in r[FIELD_TIMESTAMP:FIELD_CRC]))
        return

    t0_us = records[0][FIELD_TIMESTAMP]
    t1_us = records[-1][FIELD_TIMESTAMP]
    duration_s = (t1_us - t0_us) / 1e6
    rate_hz = (len(records) - 1) / duration_s if duration_s > 0 else float("nan")
    accel_norms_g = [
        math.sqrt(sum(a * a for a in r[FIELD_ACCEL:FIELD_ACCEL + 3])) / G_MPS2
        for r in records
    ]
    kill_count = sum(r[FIELD_KILL] for r in records)

    print(f"{path}: {len(records)} records, {duration_s:.2f} s, {rate_hz:.0f} Hz")
    print(f"accel norm [g]: min {min(accel_norms_g):.2f}, max {max(accel_norms_g):.2f}")
    print(f"kill switch engaged: {kill_count} records")
    print("use --csv for the full dump")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        pass  # piping into head/less is fine
