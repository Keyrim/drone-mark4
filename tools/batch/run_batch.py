#!/usr/bin/env python3
"""Monte Carlo throw campaign through the Godot simulator.

Starts N scenarios through `hub up sim --no-serve`, one launcher process per
instance, each on its own UDP port range and each running the real physics
and the real flight core in lockstep, faster than real time. The hub owns the
Godot import, the start order and the teardown of the pair; the campaign only
picks the ports and judges what comes back. Every run resets the world, arms,
plays a randomized throw and judges the outcome from the telemetry: reaching
a stable hover is a success, a safety cutoff or a timeout is a failure
recorded with its parameters, so any failure can be replayed alone with
--only-seed.

Arming and the kill switch travel as an RcCommandPacket stream straight to
each drone_sim command receiver, the same path a real flight uses: a
background thread per instance repeats the held state fast enough that the
flight process fail-safe never trips, whatever the time scale.

Python stdlib only. Requires the hub and drone_sim built for the desktop
preset, plus a Godot 4 binary able to open the sim-godot project (a Linux
headless build works fine inside WSL or a container).
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
    RC_MODE_ALTITUDE_AUTO,
    SIM_COMMAND_HAND_THROW,
    SIM_COMMAND_RESET,
    SIM_COMMAND_THROW,
    TUNING_ACK_OK,
    decode_telemetry,
    decode_tuning_ack,
    encode_rc_command,
    encode_sim_command,
    encode_tuning_set,
)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# mark4::FlightPhase values, carried in the telemetry.
PHASE_IDLE = 0
PHASE_ARMED = 2
PHASE_HOVER = 5
PHASE_CUTOFF = 6
PHASE_MANUAL = 7
PHASE_NAMES = [
    "idle", "altitude", "armed", "ballistic", "recovery", "hover", "cutoff", "manual",
]

# One UDP port range per instance: sim link, telemetry (+ mirror on +2),
# raw state, scenario command listener and the drone_sim command receiver
# the RC stream goes to never overlap between instances. Striding the rc
# port also keeps a campaign away from the default port a bench session
# may be using for the real board.
BASE_PORT = 48000
PORT_STRIDE = 10

# Frame budget of the flight process: large enough that a run ends because
# the campaign ended it, never because a counter ran out.
SIM_FRAMES = 4000000000

SETTLE_SIM_S = 2.0        # baro reference + attitude convergence after a reset
ARM_BUDGET_SIM_S = 2.0    # arming must be acknowledged within this
HELD_WATCH_SIM_S = 8.0    # held-only runs watch the shaken hand this long
FLIGHT_BUDGET_SIM_S = 12.0  # throw to stable hover, or the run is a failure
STABLE_HOVER_SIM_S = 3.0  # continuous hover time declaring success
TUNING_BUDGET_SIM_S = 1.0 # every --set must be acknowledged within this
WALL_BUDGET_S = 180.0     # hard wall-clock guard per run (hung pair)
LAUNCHER_STOP_S = 10.0    # longer than the launcher's own 5 s kill grace


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
    """One hub-launched simulator pair bound to its own port range."""

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

        # One process per instance: the hub launcher imports the Godot
        # project if needed, starts Godot before the flight process, and
        # takes the whole group down when it is asked to stop. --no-serve
        # keeps it out of the way: the campaign owns the sockets.
        self.launcher = subprocess.Popen(
            [
                args.hub,
                "up",
                "sim",
                "--no-serve",
                "--headless",
                "--lockstep",
                "--godot",
                args.godot,
                "--drone-sim",
                args.drone_sim,
                "--sim-port",
                str(self.sim_port),
                "--telemetry-port",
                str(self.telemetry_port),
                "--raw-port",
                str(self.raw_port),
                "--command-port",
                str(self.command_port),
                "--rc-port",
                str(self.rc_port),
                "--time-scale",
                str(args.time_scale),
                "--arena-radius",
                str(args.arena_radius),
                "--frames",
                str(SIM_FRAMES),
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
        self._acks = []

        # RC is a stream, not a shot: the flight process reverts to
        # kill+disarmed after 500 ms of silence, so a campaign has to keep
        # repeating the state it holds. Five sends per fail-safe window at
        # any time scale, since the window is counted in simulated time.
        self._rc_state = (0, 0, RC_MODE_ALTITUDE_AUTO, 0.0)
        self._rc_stop = threading.Event()
        self._rc_period_s = min(0.05, 0.1 / args.time_scale)
        self._rc_thread = threading.Thread(target=self._stream_rc, daemon=True)
        self._rc_thread.start()

    def _stream_rc(self) -> None:
        """Repeat the held RC state until close() stops the thread."""
        while not self._rc_stop.wait(self._rc_period_s):
            kill, arm, mode, throttle = self._rc_state
            # sendto is thread safe for datagrams: no lock needed around it.
            self.command.sendto(
                encode_rc_command(kill=kill, arm=arm, mode=mode, throttle=throttle),
                ("127.0.0.1", self.rc_port),
            )

    def set_rc(
        self,
        kill: int = 0,
        arm: int = 0,
        mode: int = RC_MODE_ALTITUDE_AUTO,
        throttle: float = 0.0,
    ) -> None:
        """Change the state the RC thread repeats (tuple assignment is atomic)."""
        self._rc_state = (kill, arm, mode, throttle)

    def close(self) -> None:
        self._rc_stop.set()
        self._rc_thread.join(timeout=5.0)
        # SIGTERM to the launcher only: it takes its own children down with
        # it, so reaping it is enough to reap the pair.
        if self.launcher.poll() is None:
            self.launcher.terminate()
        try:
            self.launcher.wait(timeout=LAUNCHER_STOP_S)
        except subprocess.TimeoutExpired:
            self.launcher.kill()
            self.launcher.wait()
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
        """Send one scenario command to the simulator (reset, throw, hand throw)."""
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
        """Block for the next telemetry sample, None on socket timeout.

        Tuning acks share the telemetry stream (the flight process answers on
        the link it already broadcasts on), so they are picked out here and
        queued for whoever is waiting on one.
        """
        try:
            datagram = self.telemetry.recv(256)
        except socket.timeout:
            return None
        sample = decode_telemetry(datagram)
        if sample is not None:
            self._last_sample = sample
            return sample
        ack = decode_tuning_ack(datagram)
        if ack is not None:
            self._acks.append(ack)
        return None

    def apply_tuning(self, settings) -> Optional[str]:
        """Write every --set value and wait for one ok ack each.

        Applied per run, after the reset and before arming: the reset
        rebuilds the flight core from scratch, so a push done once at startup
        would only ever reach the first run.
        """
        if not settings:
            return None
        self._acks.clear()
        for param_id, value in settings:
            self.command.sendto(
                encode_tuning_set(param_id, value), ("127.0.0.1", self.rc_port)
            )
        pending = {param_id for param_id, _ in settings}
        start = self._sim_time(WALL_BUDGET_S)
        if start is None:
            return "no telemetry while tuning"
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            for ack in self._acks:
                if ack.status == TUNING_ACK_OK:
                    pending.discard(ack.param_id)
                elif ack.param_id in pending:
                    return f"parameter {ack.param_id} refused, status {ack.status}"
            self._acks.clear()
            if not pending:
                return None
            if sample is not None and sample.timestamp_s - start > TUNING_BUDGET_SIM_S:
                return f"no ack for {sorted(pending)}"
        return "wall clock guard hit while tuning"

    def wait_first_sample(self, wall_budget_s: float) -> bool:
        """Wait for the pair to boot and the telemetry to flow."""
        deadline = time.monotonic() + wall_budget_s
        while time.monotonic() < deadline:
            if self.next_sample() is not None:
                return True
            # The launcher outlives its children: it exiting means the pair
            # is gone, whatever took it down.
            if self.launcher.poll() is not None:
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
        # The altitude-auto interlock wants the stick centered, and a centered
        # stick is harmless while the arm switch is off: nothing leaves IDLE
        # without it. Holding it centered through the settle means the arming
        # below is one switch flip, not a gesture the campaign has to time.
        self.set_rc(kill=0, arm=0, mode=RC_MODE_ALTITUDE_AUTO, throttle=0.5)
        self.send_command(SIM_COMMAND_RESET)
        if not self.wait_sim_seconds(SETTLE_SIM_S):
            result.outcome = "stalled"
            result.detail = "no telemetry while settling"
            return result

        failure = self.apply_tuning(args.set)
        if failure is not None:
            result.outcome = "setup-failed"
            result.detail = failure
            return result

        self.set_rc(kill=0, arm=1, mode=RC_MODE_ALTITUDE_AUTO, throttle=0.5)
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
            # Ranking phases by their numeric value is only a rough "how far
            # did it get", and MANUAL sits at 7, above every other phase,
            # purely because it was appended last. A campaign holds a centered
            # stick and never enters it, so the ranking stays honest here; a
            # scenario that flew manually would need a real ordering.
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
    parser.add_argument(
        "--hub",
        default=os.path.join(REPO_ROOT, "build", "desktop", "apps", "hub", "hub"),
        help="hub binary, used as the scenario launcher",
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
    parser.add_argument(
        "--set", action="append", default=[], metavar="ID=VALUE",
        help="write one tuning parameter before every throw, repeatable",
    )
    parser.add_argument("--csv", help="write every run to this CSV file")
    parser.add_argument(
        "--min-recovery", type=float, default=0.0,
        help="exit nonzero when the recovery rate falls under this [0, 1]",
    )
    args = parser.parse_args()

    settings = []
    for setting in args.set:
        name, separator, value = setting.partition("=")
        if not separator:
            sys.exit(f"batch: --set wants ID=VALUE, got '{setting}'")
        try:
            settings.append((int(name), float(value)))
        except ValueError:
            sys.exit(f"batch: --set wants ID=VALUE, got '{setting}'")
    args.set = settings

    if args.only_seed is not None:
        args.runs = 1
        args.parallel = 1
        args.seed = args.only_seed

    if not os.path.isfile(args.drone_sim):
        sys.exit(f"batch: drone_sim binary not found: {args.drone_sim} (build it first)")
    if not os.path.isfile(args.hub):
        sys.exit(f"batch: hub binary not found: {args.hub} (build it first)")

    parallel = max(1, min(args.parallel, args.runs))
    instances = [Instance(i, args) for i in range(parallel)]
    results: List[RunResult] = []
    lock = threading.Lock()
    try:
        if args.set:
            print(
                "batch: tuning "
                + ", ".join(f"{param_id}={value:g}" for param_id, value in args.set),
                flush=True,
            )
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
