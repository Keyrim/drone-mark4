#!/usr/bin/env python3
"""Decode a .m4bb blackbox file.

Prints a human summary by default, or every record as CSV with --csv.
Without a file argument, picks the newest .m4bb in logs/.
"""

import argparse
import math
import pathlib
import struct
import sys

# Must match BlackboxRecord in flight-core/include/flight_core/blackbox.hpp:
# version u8, timestamp u64, gyro 3f, accel 3f, baro f, kill u8,
# throttle f, arm u8, motors 4f - packed, little-endian.
RECORD = struct.Struct("<BQ3f3ffBfB4f")
RECORD_VERSION = 2
assert RECORD.size == 59, "format string out of sync with BlackboxRecord"

CSV_HEADER = (
    "timestamp_us,gyro_x_rad_s,gyro_y_rad_s,gyro_z_rad_s,"
    "accel_x_mps2,accel_y_mps2,accel_z_mps2,baro_pa,kill_switch,throttle,arm_switch,"
    "motor_0,motor_1,motor_2,motor_3"
)
G_MPS2 = 9.80665


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
    trailing = len(data) % RECORD.size
    if trailing:
        print(f"warning: {trailing} trailing bytes ignored (truncated record)", file=sys.stderr)

    records = list(RECORD.iter_unpack(data[: len(data) - trailing]))
    if not records:
        sys.exit(f"{path}: empty log")
    bad = next((r for r in records if r[0] != RECORD_VERSION), None)
    if bad is not None:
        sys.exit(f"{path}: unsupported record version {bad[0]} (expected {RECORD_VERSION})")

    if args.csv:
        print(CSV_HEADER)
        for r in records:
            print(",".join(repr(v) for v in r[1:]))
        return

    t0_us, t1_us = records[0][1], records[-1][1]
    duration_s = (t1_us - t0_us) / 1e6
    rate_hz = (len(records) - 1) / duration_s if duration_s > 0 else float("nan")
    accel_norms_g = [math.sqrt(r[5] ** 2 + r[6] ** 2 + r[7] ** 2) / G_MPS2 for r in records]
    kill_count = sum(r[9] for r in records)

    print(f"{path}: {len(records)} records, {duration_s:.2f} s, {rate_hz:.0f} Hz")
    print(f"accel norm [g]: min {min(accel_norms_g):.2f}, max {max(accel_norms_g):.2f}")
    print(f"kill switch engaged: {kill_count} records")
    print("use --csv for the full dump")


if __name__ == "__main__":
    try:
        main()
    except BrokenPipeError:
        pass  # piping into head/less is fine

