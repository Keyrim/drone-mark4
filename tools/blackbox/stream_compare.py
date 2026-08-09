#!/usr/bin/env python3
"""Score a recorded session: estimated state (telemetry) against the exact
simulator state (sim raw), aligned by simulated timestamp.

Prints overall RMS / max errors and the worst one-second windows, so the bad
segments of a run can be localized. Takes the CSV pair the hub records
(`hub serve --record`, or a `record` message on its websocket endpoint);
without arguments, picks the newest pair in logs/.
"""

import argparse
import csv
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent / ".."))
from telemetry_wire import error_angle_deg

#: A telemetry sample matches the closest sim raw sample within this delta.
MAX_ALIGN_US = 30000


def load(path):
    with open(path, newline="") as handle:
        return [tuple(float(cell) for cell in row) for row in list(csv.reader(handle))[1:]]


def newest_pair():
    recordings = sorted(pathlib.Path("logs").glob("streams_*_telemetry.csv"))
    if not recordings:
        sys.exit("no streams_*_telemetry.csv in logs/, record a session first")
    telemetry = recordings[-1]
    return telemetry, pathlib.Path(str(telemetry).replace("_telemetry", "_simraw"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("telemetry", nargs="?", help="telemetry CSV (default: newest in logs/)")
    parser.add_argument("simraw", nargs="?", help="matching sim raw CSV")
    args = parser.parse_args()

    if args.telemetry and args.simraw:
        telemetry_path, sim_raw_path = args.telemetry, args.simraw
    else:
        telemetry_path, sim_raw_path = newest_pair()

    telemetry = load(telemetry_path)
    sim_raw = load(sim_raw_path)
    if not telemetry or not sim_raw:
        sys.exit("empty recording")

    # Both streams share the simulated clock; walk them in timestamp order.
    errors = []  # (t_s, attitude_deg, altitude_m, vz_mps)
    raw_index = 0
    for row in telemetry:
        timestamp = row[0]
        while (raw_index + 1 < len(sim_raw)
               and abs(sim_raw[raw_index + 1][0] - timestamp)
               <= abs(sim_raw[raw_index][0] - timestamp)):
            raw_index += 1
        raw = sim_raw[raw_index]
        if abs(raw[0] - timestamp) > MAX_ALIGN_US:
            continue
        attitude_err = error_angle_deg(row[4:8], raw[1:5])
        altitude_err = row[15] - raw[7]
        vz_err = row[16] - raw[10]
        errors.append((timestamp * 1e-6, attitude_err, altitude_err, vz_err))

    if not errors:
        sys.exit("no aligned samples (were both streams recorded together?)")

    names = ("attitude [deg]", "altitude [m]", "vz [m/s]")
    print(f"{len(errors)} aligned samples over {errors[-1][0] - errors[0][0]:.1f} s "
          f"({telemetry_path})\n")
    print(f"{'metric':<16} {'RMS':>8} {'max':>8}")
    for index, name in enumerate(names, start=1):
        values = [e[index] for e in errors]
        rms = math.sqrt(sum(v * v for v in values) / len(values))
        print(f"{name:<16} {rms:>8.3f} {max(abs(v) for v in values):>8.3f}")

    print("\nworst one-second windows per metric:")
    start = errors[0][0]
    windows = {}
    for entry in errors:
        windows.setdefault(int(entry[0] - start), []).append(entry)
    for index, name in enumerate(names, start=1):
        scored = sorted(
            ((math.sqrt(sum(e[index] ** 2 for e in bucket) / len(bucket)), second)
             for second, bucket in windows.items()),
            reverse=True,
        )
        top = ", ".join(f"t={start + second:.0f}s ({rms:.2f})" for rms, second in scored[:3])
        print(f"  {name:<16} {top}")


if __name__ == "__main__":
    main()
