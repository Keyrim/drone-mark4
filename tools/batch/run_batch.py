#!/usr/bin/env python3
"""Monte Carlo throw campaign through the Godot simulator.

Spawns N (godot headless, drone_sim) pairs on separate UDP port ranges, each
running the real physics and the real flight core in lockstep, faster than
real time. Every run resets the world, arms, plays a randomized throw and
judges the outcome from the telemetry: reaching a stable hover is a success,
a safety cutoff or a timeout is a failure recorded with its parameters, so
any failure can be replayed alone with --only-seed.

Python stdlib only. Requires a Godot 4 binary able to open the sim-godot
project (a Linux headless build works fine inside WSL or a container).
"""

import argparse
import csv
import math
import os
import random
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "ground-station"))
from telemetry_wire import (
    SIM_COMMAND_HAND_THROW,
    SIM_COMMAND_RC,
    SIM_COMMAND_RESET,
    SIM_COMMAND_THROW,
    decode_telemetry,
    encode_sim_command,
)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# mark4::FlightPhase values, carried in the telemetry.
PHASE_IDLE = 0
PHASE_ARMED = 2
PHASE_HOVER = 5
PHASE_CUTOFF = 6
PHASE_NAMES = ["idle", "manual", "armed", "ballistic", "recovery", "hover", "cutoff"]

# One UDP port range per instance: sim link, telemetry (+ mirror on +2),
# raw state, command listener and rc uplink never overlap between
# instances. The rc port override is defense in depth: the uplink already
# refuses to run headless, and even if that guard regressed the stream
# must not land on the real-board bridge port.
BASE_PORT = 48000
PORT_STRIDE = 10

SETTLE_SIM_S = 2.0        # baro reference + attitude convergence after a reset
ARM_BUDGET_SIM_S = 2.0    # arming must be acknowledged within this
HELD_WATCH_SIM_S = 8.0    # held-only runs watch the shaken hand this long
FLIGHT_BUDGET_SIM_S = 12.0  # throw to stable hover, or the run is a failure
STABLE_HOVER_SIM_S = 3.0  # continuous hover time declaring success
WALL_BUDGET_S = 180.0     # hard wall-clock guard per run (hung pair)


@dataclass
class RunResult:
    """Outcome of one randomized throw."""

    run: int
    seed: int
    vz: float
    vx: float
    vy: float
    wx: float
    wy: float
    wz: float
    outcome: str = "unknown"
    detail: str = ""
    release_vz: float = 0.0
    apex_alt: float = 0.0

    def params_text(self) -> str:
        return (
            f"v=({self.vx:+.1f} {self.vy:+.1f} {self.vz:+.1f}) m/s "
            f"w=({self.wx:+.1f} {self.wy:+.1f} {self.wz:+.1f}) rad/s"
        )


class Instance:
    """One (godot headless, drone_sim) pair bound to its own port range."""

    def __init__(self, index: int, args: argparse.Namespace):
        self.index = index
        base = BASE_PORT + index * PORT_STRIDE
        self.sim_port = base
        self.telemetry_port = base + 1
        self.raw_port = base + 5
        self.command_port = base + 7
        self.rc_port = base + 9

        log_dir = os.path.join(REPO_ROOT, "logs")
        os.makedirs(log_dir, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        self._log = open(
            os.path.join(log_dir, f"batch_{stamp}_i{index}.log"), "w", encoding="utf-8"
        )

        # Godot first: its lockstep resends cover the gap until drone_sim is
        # up, while drone_sim would give up after its idle timeout if it had
        # to wait for a slow Godot boot.
        self.godot = subprocess.Popen(
            [
                args.godot,
                "--headless",
                "--path",
                os.path.join(REPO_ROOT, "sim-godot"),
                "--",
                "--flight-port",
                str(self.sim_port),
                "--command-port",
                str(self.command_port),
                "--raw-port",
                str(self.raw_port),
                "--rc-port",
                str(self.rc_port),
                "--lockstep",
                "--time-scale",
                str(args.time_scale),
                "--arena-radius",
                str(args.arena_radius),
            ],
            stdout=self._log,
            stderr=subprocess.STDOUT,
            cwd=REPO_ROOT,
        )
        time.sleep(0.5)
        self.drone_sim = subprocess.Popen(
            [
                args.drone_sim,
                "4000000000",
                "--sim-port",
                str(self.sim_port),
                "--telemetry-port",
                str(self.telemetry_port),
            ],
            stdout=self._log,
            stderr=subprocess.STDOUT,
            cwd=REPO_ROOT,
        )

        self.telemetry = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.telemetry.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.telemetry.bind(("0.0.0.0", self.telemetry_port))
        self.telemetry.settimeout(1.0)

        self.command = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._last_sample = None

    def close(self) -> None:
        for process in (self.godot, self.drone_sim):
            if process.poll() is None:
                process.terminate()
        for process in (self.godot, self.drone_sim):
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
        self.telemetry.close()
        self.command.close()
        self._log.close()

    def send_command(
        self,
        command: int,
        kill: int = 0,
        arm: int = 0,
        throttle: float = 0.0,
        velocity=(0.0, 0.0, 0.0),
        angular=(0.0, 0.0, 0.0),
        held_s: float = 0.0,
        held_tilt: float = 0.0,
        held_azimuth: float = 0.0,
        swing_s: float = 0.0,
    ) -> None:
        packet = encode_sim_command(
            command, kill=kill, arm=arm, throttle=throttle,
            velocity=velocity, angular=angular,
            held_s=held_s, held_tilt=held_tilt,
            held_azimuth=held_azimuth, swing_s=swing_s,
        )
        self.command.sendto(packet, ("127.0.0.1", self.command_port))

    def drain_telemetry(self) -> None:
        """Discard buffered samples so judgments only see fresh ones."""
        self.telemetry.setblocking(False)
        try:
            while True:
                self.telemetry.recv(256)
        except BlockingIOError:
            pass
        finally:
            self.telemetry.settimeout(1.0)

    def next_sample(self):
        """Block for the next telemetry sample, None on socket timeout."""
        try:
            datagram = self.telemetry.recv(256)
        except socket.timeout:
            return None
        sample = decode_telemetry(datagram)
        if sample is not None:
            self._last_sample = sample
        return sample

    def wait_first_sample(self, wall_budget_s: float) -> bool:
        """Wait for the pair to boot and the telemetry to flow."""
        deadline = time.monotonic() + wall_budget_s
        while time.monotonic() < deadline:
            if self.next_sample() is not None:
                return True
            if self.godot.poll() is not None or self.drone_sim.poll() is not None:
                return False
        return False

    def wait_sim_seconds(self, duration_s: float) -> bool:
        """Let the pair advance by the given simulated time."""
        start = self._sim_time(WALL_BUDGET_S)
        if start is None:
            return False
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is not None and sample.timestamp_s - start >= duration_s:
                return True
        return False

    def _sim_time(self, wall_budget_s: float) -> Optional[float]:
        deadline = time.monotonic() + wall_budget_s
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is not None:
                return sample.timestamp_s
        return None

    def run_one(self, run: int, seed: int, args) -> RunResult:
        rng = random.Random(seed)
        vz = rng.uniform(4.0, 8.0)
        horizontal = rng.uniform(0.0, 3.0)
        azimuth = rng.uniform(0.0, 2.0 * math.pi)
        vx = horizontal * math.cos(azimuth)
        vy = horizontal * math.sin(azimuth)
        wx = rng.uniform(-6.0, 6.0)
        wy = rng.uniform(-6.0, 6.0)
        wz = rng.uniform(-6.0, 6.0)
        # Hand parameters: an arbitrary held attitude, a human hold, a real
        # arm swing. The wrist spin is gentler than the instant-throw tumble:
        # the arc itself already rotates the drone at release.
        held_s = rng.uniform(1.0, 2.5)
        held_tilt = rng.uniform(0.0, math.radians(70.0))
        held_azimuth = rng.uniform(0.0, 2.0 * math.pi)
        swing_s = rng.uniform(0.25, 0.45)
        result = RunResult(run=run, seed=seed, vz=vz, vx=vx, vy=vy, wx=wx, wy=wy, wz=wz)

        # A fresh world and a fresh flight core, then let the estimators
        # settle exactly like a drone powered up on the ground.
        self.drain_telemetry()
        self.send_command(SIM_COMMAND_RC, kill=0, arm=0, throttle=0.0)
        self.send_command(SIM_COMMAND_RESET)
        if not self.wait_sim_seconds(SETTLE_SIM_S):
            result.outcome = "stalled"
            result.detail = "no telemetry while settling"
            return result

        self.send_command(SIM_COMMAND_RC, kill=0, arm=1, throttle=0.0)
        if not self._wait_phase(PHASE_ARMED, ARM_BUDGET_SIM_S):
            result.outcome = "setup-failed"
            result.detail = "arming was not acknowledged"
            return result

        if args.held_only:
            self.send_command(
                SIM_COMMAND_HAND_THROW,
                held_s=999.0,
                held_tilt=held_tilt,
                held_azimuth=held_azimuth,
                swing_s=0.0,
            )
            self._judge_held(result)
            return result

        if args.hand:
            self.send_command(
                SIM_COMMAND_HAND_THROW,
                velocity=(vx, vy, vz),
                angular=(wx / 3.0, wy / 3.0, wz / 3.0),
                held_s=held_s,
                held_tilt=held_tilt,
                held_azimuth=held_azimuth,
                swing_s=swing_s,
            )
            self._judge_flight(result, extra_budget_s=1.0 + held_s + swing_s)
        else:
            self.send_command(SIM_COMMAND_THROW, velocity=(vx, vy, vz), angular=(wx, wy, wz))
            self._judge_flight(result)
        return result

    def _judge_held(self, result: RunResult) -> None:
        """A shaken hand must never start the propellers: watch for a while."""
        start = self._sim_time(WALL_BUDGET_S)
        if start is None:
            result.outcome = "stalled"
            result.detail = "no telemetry while held"
            return
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None:
                continue
            if sample.throw_count > 0 or max(sample.motor) > 0.01:
                result.outcome = "SPUN-UP"
                result.detail = (
                    f"throw count {sample.throw_count}, motors {max(sample.motor):.2f}"
                )
                return
            if sample.timestamp_s - start > HELD_WATCH_SIM_S:
                result.outcome = "no-spinup"
                result.detail = f"quiet for {HELD_WATCH_SIM_S:.0f} s in hand"
                return
        result.outcome = "stalled"
        result.detail = "wall clock guard hit"

    def _wait_phase(self, phase: int, budget_sim_s: float) -> bool:
        start = self._sim_time(WALL_BUDGET_S)
        if start is None:
            return False
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None:
                continue
            if sample.flight_phase == phase:
                return True
            if sample.timestamp_s - start > budget_sim_s:
                return False
        return False

    def _judge_flight(self, result: RunResult, extra_budget_s: float = 0.0) -> None:
        start = self._sim_time(WALL_BUDGET_S)
        if start is None:
            result.outcome = "stalled"
            result.detail = "no telemetry after the throw"
            return
        budget_s = FLIGHT_BUDGET_SIM_S + extra_budget_s
        hover_since: Optional[float] = None
        highest_phase = PHASE_IDLE
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None:
                continue
            highest_phase = max(highest_phase, sample.flight_phase)
            result.release_vz = sample.release_velocity_mps
            result.apex_alt = sample.apex_altitude_m
            if sample.flight_phase == PHASE_CUTOFF:
                result.outcome = "cutoff"
                result.detail = "safety cutoff"
                return
            if sample.flight_phase == PHASE_HOVER:
                if hover_since is None:
                    hover_since = sample.timestamp_s
                elif sample.timestamp_s - hover_since >= STABLE_HOVER_SIM_S:
                    result.outcome = "recovered"
                    result.detail = f"hover stable for {STABLE_HOVER_SIM_S:.0f} s"
                    return
            else:
                hover_since = None
            if sample.timestamp_s - start > budget_s:
                result.outcome = "timeout"
                result.detail = f"best phase reached: {PHASE_NAMES[highest_phase]}"
                return
        result.outcome = "stalled"
        result.detail = "wall clock guard hit"


def worker(instance: Instance, jobs: List[int], args, results: List[RunResult], lock):
    ok_outcomes = ("no-spinup",) if args.held_only else ("recovered",)
    for run in jobs:
        result = instance.run_one(run, args.seed + run, args)
        with lock:
            results.append(result)
            marker = "ok " if result.outcome in ok_outcomes else "FAIL"
            print(
                f"[{marker}] run {result.run:4d} seed {result.seed:8d} "
                f"{result.params_text()} -> {result.outcome} ({result.detail})",
                flush=True,
            )


def ensure_import(godot: str) -> None:
    """First launch of a fresh checkout: let Godot import its resources."""
    project = os.path.join(REPO_ROOT, "sim-godot")
    if os.path.isdir(os.path.join(project, ".godot")):
        return
    print("batch: importing the Godot project (first run)...", flush=True)
    subprocess.run([godot, "--headless", "--path", project, "--import"], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=100, help="number of throws")
    parser.add_argument("--parallel", type=int, default=4, help="simulator pairs")
    parser.add_argument("--seed", type=int, default=42, help="base seed, run i uses seed+i")
    parser.add_argument("--only-seed", type=int, help="replay one exact seed, single run")
    parser.add_argument("--godot", default="godot", help="godot 4 binary")
    parser.add_argument(
        "--drone-sim",
        default=os.path.join(REPO_ROOT, "build", "desktop", "apps", "drone_sim", "drone_sim"),
        help="drone_sim binary",
    )
    parser.add_argument("--time-scale", type=float, default=20.0, help="sim speed factor")
    parser.add_argument(
        "--arena-radius", type=float, default=0.0,
        help="circular wall around the launch point [m], 0 = open field",
    )
    parser.add_argument(
        "--hand", action="store_true",
        help="throw with the simulated hand (held, swayed, swung) instead of instantly",
    )
    parser.add_argument(
        "--held-only", action="store_true",
        help="never throw: hold and shake, measure the false spin-up rate",
    )
    parser.add_argument("--csv", help="write every run to this CSV file")
    parser.add_argument(
        "--min-recovery", type=float, default=0.0,
        help="exit nonzero when the recovery rate falls under this [0, 1]",
    )
    args = parser.parse_args()

    if args.only_seed is not None:
        args.runs = 1
        args.parallel = 1
        args.seed = args.only_seed

    if not os.path.isfile(args.drone_sim):
        sys.exit(f"batch: drone_sim binary not found: {args.drone_sim} (build it first)")
    ensure_import(args.godot)

    parallel = max(1, min(args.parallel, args.runs))
    instances = [Instance(i, args) for i in range(parallel)]
    results: List[RunResult] = []
    lock = threading.Lock()
    try:
        print(f"batch: waiting for {parallel} simulator pair(s) to boot...", flush=True)
        for instance in instances:
            if not instance.wait_first_sample(60.0):
                sys.exit(f"batch: instance {instance.index} never produced telemetry")

        threads = []
        for i, instance in enumerate(instances):
            jobs = list(range(i, args.runs, parallel))
            thread = threading.Thread(
                target=worker, args=(instance, jobs, args, results, lock), daemon=True
            )
            thread.start()
            threads.append(thread)
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        print("\nbatch: interrupted, partial results below", flush=True)
    finally:
        for instance in instances:
            instance.close()

    results.sort(key=lambda r: r.run)
    good = "no-spinup" if args.held_only else "recovered"
    recovered = sum(1 for r in results if r.outcome == good)
    rate = recovered / len(results) if results else 0.0
    print(f"\nbatch: {recovered}/{len(results)} {good} ({100.0 * rate:.1f}%)")
    for outcome in ("SPUN-UP", "cutoff", "timeout", "setup-failed", "stalled"):
        hits = [r for r in results if r.outcome == outcome]
        if hits:
            print(f"  {outcome}: {len(hits)}")
            for r in hits:
                print(f"    seed {r.seed}  {r.params_text()}  {r.detail}")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(
                "run seed vx vy vz wx wy wz outcome detail release_vz apex_alt".split()
            )
            for r in results:
                writer.writerow(
                    [r.run, r.seed, r.vx, r.vy, r.vz, r.wx, r.wy, r.wz,
                     r.outcome, r.detail, r.release_vz, r.apex_alt]
                )
        print(f"batch: results written to {args.csv}")

    return 0 if rate >= args.min_recovery else 1


if __name__ == "__main__":
    sys.exit(main())
