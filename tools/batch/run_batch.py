#!/usr/bin/env python3
"""Monte Carlo throw campaign through the Godot simulator.

Starts N simulator pairs (headless Godot plus drone_sim), each on its own
UDP port range and each running the real physics and the real flight core in
lockstep, faster than real time. The campaign spawns and tears down the pairs
itself; the hub is a bench tool and plays no part here.

One run is one scenario packet. The packet opens with a reset and carries
everything the run needs - the seed, the delay before the throw, the throw
itself - so the whole run is scripted inside the plant, on the plant's own
tick grid, counted from the reset tick. Nothing here has to agree with the
plant on which absolute tick anything happened at. The packet goes to the
flight process command receiver, which forwards the block to the plant inside
the lockstep reply; resending it is free, since the plant plays a block once
per change of its sequence byte.

The flight process hashes the trajectory of every run and broadcasts the hash
on the telemetry port. Two runs given the same scenario must produce the same
hash, which is what --verify-repro checks.

Arming and the kill switch travel as an RcCommandPacket stream straight to
each drone_sim command receiver, the same path a real flight uses: a
background thread per instance repeats the held state fast enough that the
flight process fail-safe never trips, whatever the time scale. The state is
held constant for the whole campaign - armed, altitude-auto, stick centered -
which is what the altitude-auto interlock wants and what makes the throw path
reachable.

--set writes tuning parameters once per run, after the reset that rebuilt the
flight core and before the throw, so a sweep measures the value it names.

Python stdlib only. Requires drone_sim built for the desktop preset, plus a
Godot 4 binary able to open the sim-godot project (a Linux headless build
works fine inside WSL or a container).
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
from dataclasses import dataclass
from typing import List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from telemetry_wire import (
    RC_MODE_ALTITUDE_AUTO,
    SIM_SCENARIO_HAND_THROW,
    SIM_SCENARIO_THROW,
    TUNING_ACK_OK,
    decode_sim_run_stats,
    decode_telemetry,
    decode_tuning_ack,
    encode_rc_command,
    encode_sim_scenario,
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

# One UDP port range per instance: sim link, telemetry, raw state and the
# drone_sim command receiver never overlap between instances.
# Striding the rc port also keeps a campaign away from the default port a
# bench session may be using for the real board.
BASE_PORT = 48000
PORT_STRIDE = 10

THROW_DELAY_SIM_S = 2.0   # reset to throw: baro reference + attitude convergence
HELD_WATCH_SIM_S = 8.0    # held-only runs watch the shaken hand this long
FLIGHT_BUDGET_SIM_S = 12.0  # throw to stable hover, or the run is a failure
STABLE_HOVER_SIM_S = 3.0  # continuous hover time declaring success
TUNING_BUDGET_SIM_S = 1.0 # every --set must be acknowledged within this
WALL_BUDGET_S = 180.0     # hard wall-clock guard per run (hung pair)
PAIR_STOP_S = 10.0        # SIGTERM grace before a pair member is killed
GODOT_SETTLE_S = 0.5      # godot boots first, it resends until answered
SCENARIO_RETRY_S = 0.2    # resend period until the plant acknowledges a run

# Simulated time the flight process hashes from the reset tick: the whole run
# it is going to be judged on, plus a margin.
HASH_WINDOW_SIM_S = THROW_DELAY_SIM_S + FLIGHT_BUDGET_SIM_S + 2.0


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
    run_id: int = -1
    traj_hash: str = ""
    degraded: bool = False

    def params_text(self) -> str:
        return (
            f"v=({self.vx:+.1f} {self.vy:+.1f} {self.vz:+.1f}) m/s "
            f"w=({self.wx:+.1f} {self.wy:+.1f} {self.wz:+.1f}) rad/s"
        )


class Instance:
    """One simulator pair (godot + drone_sim) bound to its own port range."""

    def __init__(self, index: int, args: argparse.Namespace):
        self.index = index
        base = BASE_PORT + index * PORT_STRIDE
        self.sim_port = base
        self.telemetry_port = base + 1
        self.raw_port = base + 5
        self.rc_port = base + 9

        log_dir = os.path.join(REPO_ROOT, "logs", "batch")
        os.makedirs(log_dir, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        self._log = open(
            os.path.join(log_dir, f"batch_{stamp}_i{index}.log"), "w", encoding="utf-8"
        )

        # Godot first: it resends until the flight process answers, while
        # the flight process would sit on an empty socket through a slow
        # Godot boot and eat into the boot budget.
        self.plant = subprocess.Popen(
            [
                args.godot,
                "--headless",
                "--path",
                os.path.join(REPO_ROOT, "sim-godot"),
                "--",
                "--flight-port",
                str(self.sim_port),
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
        time.sleep(GODOT_SETTLE_S)
        self.flight = subprocess.Popen(
            [
                args.drone_sim,
                "--sim-port",
                str(self.sim_port),
                "--telemetry-port",
                str(self.telemetry_port),
                "--rc-port",
                str(self.rc_port),
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
        #: Last run stats packet decoded, the verdict of the current run.
        self.last_stats = None
        self._scenario_sequence = 0
        self._acks = []

        # RC is a stream, not a shot: the flight process reverts to
        # kill+disarmed after 500 ms of silence, so a campaign has to keep
        # repeating the state it holds. Five sends per fail-safe window at
        # any time scale, since the window is counted in simulated time.
        #
        # It is held for the WHOLE campaign, set once here and never touched
        # again: kill released, armed, altitude-auto, stick centered. The
        # centered stick is what the altitude-auto interlock wants, and
        # altitude-auto is the mode the throw path is reachable from; the
        # stick never leaves its deadband afterwards, so the core never hands
        # control back to it and the judge keeps its meaning.
        #
        # Arming still cannot move the plant - motors are zero in IDLE, ARMED
        # and BALLISTIC, and the estimators do not depend on the phase - so
        # the tick the arm state lands on has no effect on the trajectory,
        # and the trajectory hash stays comparable even though RC is paced by
        # this host rather than by the plant.
        self._rc_state = (0, 1, RC_MODE_ALTITUDE_AUTO, 0.5)
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
        for child in (self.flight, self.plant):
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=PAIR_STOP_S)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        self.telemetry.close()
        self.command.close()
        self._log.close()

    def send_scenario(
        self,
        scenario: int,
        seed: int = 0,
        throw_delay_us: int = 0,
        hash_window_us: int = 0,
        velocity=(0.0, 0.0, 0.0),
        angular=(0.0, 0.0, 0.0),
        held_s: float = 0.0,
        held_tilt: float = 0.0,
        held_azimuth: float = 0.0,
        swing_s: float = 0.0,
    ) -> bytes:
        """Send one scenario to the flight process, and return its bytes.

        The sequence byte is what makes the send idempotent: the plant plays
        a block once per change of it, so the caller may resend the returned
        bytes until it sees the run start.
        """
        self._scenario_sequence = self._scenario_sequence % 255 + 1
        packet = encode_sim_scenario(
            self._scenario_sequence, scenario, seed=seed,
            throw_delay_us=throw_delay_us, hash_window_us=hash_window_us,
            velocity=velocity, angular=angular,
            held_s=held_s, held_tilt=held_tilt,
            held_azimuth=held_azimuth, swing_s=swing_s,
        )
        self.command.sendto(packet, ("127.0.0.1", self.rc_port))
        return packet

    def resend(self, packet: bytes) -> None:
        """Resend a scenario packet unchanged; the plant dedups on sequence."""
        self.command.sendto(packet, ("127.0.0.1", self.rc_port))

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
        """Block for the next telemetry sample, None on anything else.

        Three streams share the telemetry port, because the flight process
        answers on the link it already broadcasts on. Run stats are absorbed
        into last_stats and tuning acks are queued for whoever is waiting on
        one; neither is ever returned, so a caller judging a flight only ever
        sees telemetry.
        """
        try:
            datagram = self.telemetry.recv(256)
        except socket.timeout:
            return None
        stats = decode_sim_run_stats(datagram)
        if stats is not None:
            self.last_stats = stats
            return None
        ack = decode_tuning_ack(datagram)
        if ack is not None:
            self._acks.append(ack)
            return None
        return decode_telemetry(datagram)

    def apply_tuning(self, settings, run_start_s: float) -> Optional[str]:
        """Write every --set value and wait for one ok ack each.

        Applied per run, after the reset that opened it: the reset rebuilds
        the flight core from scratch and restores the tuning defaults with
        it, so a push done once at startup would only ever reach the first
        run.

        @return None when every value was acknowledged, the reason otherwise.
        """
        if not settings:
            return None
        self._acks.clear()
        for param_id, value in settings:
            self.command.sendto(
                encode_tuning_set(param_id, value), ("127.0.0.1", self.rc_port)
            )
        pending = {param_id for param_id, _ in settings}
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
            if sample is not None and sample.timestamp_s - run_start_s > TUNING_BUDGET_SIM_S:
                return f"no ack for {sorted(pending)}"
        return "wall clock guard hit while tuning"

    def wait_first_sample(self, wall_budget_s: float) -> bool:
        """Wait for the pair to boot, the telemetry to flow and stats to say
        which run is current."""
        deadline = time.monotonic() + wall_budget_s
        telemetry_seen = False
        while time.monotonic() < deadline:
            if self.next_sample() is not None:
                telemetry_seen = True
            if telemetry_seen and self.last_stats is not None:
                return True
            # Either member exiting means the pair is gone, whatever took
            # it down.
            if self.plant.poll() is not None or self.flight.poll() is not None:
                return False
        return False

    def start_run(self, packet: bytes, previous_run_id: int) -> bool:
        """Resend a scenario until the plant reports a new run."""
        deadline = time.monotonic() + WALL_BUDGET_S
        next_retry = time.monotonic() + SCENARIO_RETRY_S
        while time.monotonic() < deadline:
            self.next_sample()
            if self.last_stats is not None and self.last_stats.run_id != previous_run_id:
                return True
            if time.monotonic() >= next_retry:
                self.resend(packet)
                next_retry = time.monotonic() + SCENARIO_RETRY_S
        return False

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

        throw_delay_us = int(THROW_DELAY_SIM_S * 1e6)
        hash_window_us = int(HASH_WINDOW_SIM_S * 1e6)
        # Read before sending: the plant may acknowledge the scenario before
        # the next line runs, and the comparison would then never fire.
        previous_run_id = self.last_stats.run_id if self.last_stats is not None else -1
        if args.held_only:
            packet = self.send_scenario(
                SIM_SCENARIO_HAND_THROW, seed=seed,
                throw_delay_us=throw_delay_us, hash_window_us=hash_window_us,
                held_s=999.0, held_tilt=held_tilt, held_azimuth=held_azimuth,
                swing_s=0.0,
            )
        elif args.hand:
            packet = self.send_scenario(
                SIM_SCENARIO_HAND_THROW, seed=seed,
                throw_delay_us=throw_delay_us, hash_window_us=hash_window_us,
                velocity=(vx, vy, vz), angular=(wx / 3.0, wy / 3.0, wz / 3.0),
                held_s=held_s, held_tilt=held_tilt, held_azimuth=held_azimuth,
                swing_s=swing_s,
            )
        else:
            packet = self.send_scenario(
                SIM_SCENARIO_THROW, seed=seed,
                throw_delay_us=throw_delay_us, hash_window_us=hash_window_us,
                velocity=(vx, vy, vz), angular=(wx, wy, wz),
            )

        if not self.start_run(packet, previous_run_id):
            result.outcome = "stalled"
            result.detail = "the plant never acknowledged the scenario"
            return result
        result.run_id = self.last_stats.run_id
        run_start_s = self.last_stats.run_start_s

        # After the reset that wiped the previous values, and before the
        # throw: a sweep that missed its window would fly the defaults and
        # report them as the swept gain.
        failure = self.apply_tuning(args.set, run_start_s)
        if failure is not None:
            result.outcome = "setup-failed"
            result.detail = failure
            return result

        # Arming is out-of-band and held for the whole campaign, so it is
        # only ever a harness problem when it does not show up.
        if not self._wait_phase(PHASE_ARMED, run_start_s, THROW_DELAY_SIM_S):
            result.outcome = "setup-failed"
            result.detail = "arming was not acknowledged before the throw"
            return result

        if args.held_only:
            self._judge_held(result, run_start_s)
        elif args.hand:
            self._judge_flight(
                result, run_start_s, extra_budget_s=1.0 + held_s + swing_s
            )
        else:
            self._judge_flight(result, run_start_s)

        self._collect_hash(result)
        return result

    def _collect_hash(self, result: RunResult) -> None:
        """Wait for the hash of this run to seal, then record it."""
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            stats = self.last_stats
            if stats is not None and stats.run_id == result.run_id:
                result.degraded = result.degraded or stats.degraded
                if stats.sealed:
                    result.traj_hash = f"{stats.run_hash:016x}"
                    return
            self.next_sample()
        result.detail += " (hash never sealed)"

    def _judge_held(self, result: RunResult, run_start_s: float) -> None:
        """A shaken hand must never start the propellers: watch for a while."""
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None or sample.timestamp_s < run_start_s:
                continue
            if sample.throw_count > 0 or max(sample.motor) > 0.01:
                result.outcome = "SPUN-UP"
                result.detail = (
                    f"throw count {sample.throw_count}, motors {max(sample.motor):.2f}"
                )
                return
            if sample.timestamp_s - run_start_s > THROW_DELAY_SIM_S + HELD_WATCH_SIM_S:
                result.outcome = "no-spinup"
                result.detail = f"quiet for {HELD_WATCH_SIM_S:.0f} s in hand"
                return
        result.outcome = "stalled"
        result.detail = "wall clock guard hit"

    def _wait_phase(self, phase: int, run_start_s: float, budget_sim_s: float) -> bool:
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None or sample.timestamp_s < run_start_s:
                continue
            if sample.flight_phase == phase:
                return True
            if sample.timestamp_s - run_start_s > budget_sim_s:
                return False
        return False

    def _judge_flight(
        self, result: RunResult, run_start_s: float, extra_budget_s: float = 0.0
    ) -> None:
        budget_s = THROW_DELAY_SIM_S + FLIGHT_BUDGET_SIM_S + extra_budget_s
        hover_since: Optional[float] = None
        highest_phase = PHASE_IDLE
        deadline = time.monotonic() + WALL_BUDGET_S
        while time.monotonic() < deadline:
            sample = self.next_sample()
            if sample is None or sample.timestamp_s < run_start_s:
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
            if sample.timestamp_s - run_start_s > budget_s:
                result.outcome = "timeout"
                result.detail = f"best phase reached: {PHASE_NAMES[highest_phase]}"
                return
        result.outcome = "stalled"
        result.detail = "wall clock guard hit"


def worker(instance: Instance, jobs: List[int], args, results: List[RunResult], lock):
    ok_outcomes = ("no-spinup",) if args.held_only else ("recovered",)
    for run in jobs:
        # --verify-repro plays the SAME draw twice, in the same instance:
        # anything but the same hash is a determinism bug.
        seed = args.seed if args.verify_repro else args.seed + run
        result = instance.run_one(run, seed, args)
        with lock:
            results.append(result)
            marker = "ok " if result.outcome in ok_outcomes else "FAIL"
            print(
                f"[{marker}] run {result.run:4d} seed {result.seed:8d} "
                f"{result.params_text()} -> {result.outcome} ({result.detail}) "
                f"hash {result.traj_hash or 'none'}"
                f"{' DEGRADED' if result.degraded else ''}",
                flush=True,
            )


def report_repro(results: List[RunResult]) -> int:
    """Compare the hashes of the two runs of a --verify-repro campaign."""
    print("\nbatch: reproducibility check")
    for r in results:
        print(f"  run {r.run} seed {r.seed} -> {r.outcome}, hash {r.traj_hash or 'none'}")
    if len(results) != 2 or not all(r.traj_hash for r in results):
        print("batch: FAILED, both runs must seal a hash")
        return 1
    if results[0].traj_hash != results[1].traj_hash:
        print("batch: FAILED, the same scenario produced two trajectories")
        return 1
    if any(r.degraded for r in results):
        print("batch: FAILED, the link degraded during the check")
        return 1
    print("batch: identical hashes, the run is reproducible")
    return 0


def ensure_import(godot: str) -> None:
    """Import the Godot project once, before any pair spawns.

    A fresh checkout has no .godot cache and parallel instances would race
    the import; one serialized headless import settles it for the campaign.
    """
    project = os.path.join(REPO_ROOT, "sim-godot")
    if os.path.isdir(os.path.join(project, ".godot")):
        return
    print("batch: importing the godot project (first run)...", flush=True)
    done = subprocess.run(
        [godot, "--headless", "--path", project, "--import"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
        cwd=REPO_ROOT,
        check=False,
    )
    if done.returncode != 0:
        sys.exit(f"batch: the godot import failed (code {done.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=100, help="number of throws")
    parser.add_argument("--parallel", type=int, default=4, help="simulator pairs")
    parser.add_argument("--seed", type=int, default=42, help="base seed, run i uses seed+i")
    parser.add_argument("--only-seed", type=int, help="replay one exact seed, single run")
    parser.add_argument("--godot", default="godot", help="godot 4 binary")
    parser.add_argument(
        "--drone-sim",
        default=os.path.join(REPO_ROOT, "software", "build", "desktop", "drone_sim", "drone_sim"),
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
    parser.add_argument(
        "--set", action="append", default=[], metavar="ID=VALUE",
        help="write one tuning parameter before every throw, repeatable",
    )
    parser.add_argument(
        "--verify-repro", action="store_true",
        help="play one seed twice in the same instance and compare the hashes",
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
    if args.verify_repro:
        # Two runs of the same draw, back to back, in one plant: the only
        # shape where a hash difference can only be the plant's own doing.
        args.runs = 2
        args.parallel = 1

    if not os.path.isfile(args.drone_sim):
        sys.exit(f"batch: drone_sim binary not found: {args.drone_sim} (build it first)")
    ensure_import(args.godot)

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
    if args.verify_repro:
        return report_repro(results)

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
    degraded = [r for r in results if r.degraded]
    if degraded:
        # A degraded run is not a flight result: the trajectory it produced
        # is not the one the scenario asked for.
        print(f"  degraded link: {len(degraded)}")
        for r in degraded:
            print(f"    seed {r.seed}  run {r.run_id}")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(
                "run seed vx vy vz wx wy wz outcome detail release_vz apex_alt "
                "traj_hash degraded".split()
            )
            for r in results:
                writer.writerow(
                    [r.run, r.seed, r.vx, r.vy, r.vz, r.wx, r.wy, r.wz,
                     r.outcome, r.detail, r.release_vz, r.apex_alt,
                     r.traj_hash, int(r.degraded)]
                )
        print(f"batch: results written to {args.csv}")

    broken = [r for r in results if r.outcome in ("stalled", "setup-failed") or r.degraded]
    if broken:
        # Harness or link failures are not flight results, and a campaign
        # that produced them measured nothing: say so with the exit code.
        return 1
    return 0 if rate >= args.min_recovery else 1


if __name__ == "__main__":
    sys.exit(main())
