# Monte Carlo throw campaigns

`run_batch.py` measures the recovery rate of the flight core over randomized
throws, using the real Godot physics as the single reference: it starts N
scenarios through `hub up sim --no-serve`, sends one scenario packet per run
and judges the outcome from the telemetry.

One run is one packet. It opens with a reset and carries everything the run
needs - the seed, the delay before the throw, the throw itself - so the whole
run is scripted inside the plant, on the plant's own tick grid, counted from
the reset tick. The campaign never has to agree with the plant on which
absolute tick anything happened at. The packet goes to the `drone_sim`
command receiver, which forwards the block to Godot inside the lockstep
reply; resending it is free, since the plant plays a block once per change of
its sequence byte, and the campaign resends every 200 ms until the plant
reports the new run.

One launcher process per instance, one port range per instance. The hub owns
the pair (Godot import, start order, teardown of the whole group when it is
asked to stop); the campaign only picks the ports, drives the scenario and
judges. `--no-serve` keeps the hub off the sockets: it supervises and nothing
else, so the campaign is the only reader of its telemetry port.

Arming and the kill switch do not go through the simulator: they are streamed
as `RcCommandPacket` straight at each `drone_sim` command receiver, the same
path a real flight uses. A background thread per instance repeats the held
state, fast enough that the 500 ms fail-safe never trips at any `--time-scale`
(the window is counted in simulated time). Each instance strides its own rc
port, so a campaign never lands on the port a bench session may be using.

The held state is set once, at instance construction, and never changed for
the whole campaign: kill released, armed, altitude-auto, stick centered. The
centered stick is the interlock altitude-auto is entered through, and
altitude-auto is the mode the throw path is reachable from; the stick then
never leaves its deadband, so the core never hands control back to it and the
judge keeps its meaning.

Holding it constant rests on an invariant worth stating, because it is what
keeps the trajectory hashes comparable: **the arm tick cannot move the
plant.** Motors are zero in IDLE, ARMED and BALLISTIC, and the estimators do
not depend on the flight phase, so which tick the core leaves IDLE on has no
effect on the trajectory. The per-mode interlocks did not change that: with
the RC state constant from before the reset, the interlock is already
satisfied when the rebuilt core starts looking, so IDLE -> ARMED happens the
frame `VerticalEstimator::ready()` turns true and nothing else gates it. This
is just as well, since RC is paced by this host's wall clock and not by the
plant. If arming ever gains a physical effect before the throw - or an
interlock ever needs a gesture rather than a level - this stops being true
and the arming would have to move into the scenario.

## Requirements

- `hub` and `drone_sim` built for the desktop preset. Override their paths
  with `--hub` and `--drone-sim`.
- A Godot 4 binary able to open `sim-godot/` (same version as the editor).
  Headless needs no GPU: a Linux build works inside WSL or a container.
  Pass it with `--godot /path/to/godot` if it is not on the PATH.

The first run on a fresh checkout imports the Godot project automatically
(the launcher does it). Run one instance first on such a checkout: parallel
instances would otherwise import the same project at the same time.

## Usage

```sh
# 100 throws on 4 parallel simulator pairs
python3 tools/batch/run_batch.py --runs 100 --parallel 4

# Full campaign with a CSV report and a pass/fail threshold
python3 tools/batch/run_batch.py --runs 1000 --parallel 8 \
    --csv logs/campaign.csv --min-recovery 0.9

# Replay one failure exactly as it was drawn
python3 tools/batch/run_batch.py --only-seed 1234567

# "Recover within X meters": a circular wall around the launch point
python3 tools/batch/run_batch.py --runs 200 --parallel 8 --arena-radius 10

# Realistic hand throws: held tilted and swayed, then swung along an arm arc
python3 tools/batch/run_batch.py --runs 200 --parallel 8 --hand

# Safety campaign: hold and shake, never throw - the false spin-up rate
python3 tools/batch/run_batch.py --runs 40 --parallel 4 --held-only

# Sweep a gain: the same 200 throws at three hover collectives
for value in 0.50 0.55 0.60; do
    python3 tools/batch/run_batch.py --runs 200 --parallel 8 \
        --set 303=$value --csv logs/hover_$value.csv
done
```

`--set ID=VALUE` writes one tuning parameter, repeatable, ids from
`flight-core/include/flight_core/tuning_table.hpp`. The values are applied
per run, after the world reset and before the throw: the reset rebuilds the
flight core from scratch and restores the defaults with it, so a push done
once at startup would only ever reach the first run. Every write is verified against its acknowledgement,
and a parameter that is refused (unknown id, out of bounds, locked) ends the
run as `setup-failed` with the status it came back with, rather than
silently measuring the default.

`--arena-radius` builds a translucent wall ring at the given distance (also
available in the editor: the Arena node of the main scene). Drifting into it
trips the impact cutoff, so the recovery rate becomes a function of the
allowed radius - sweep it to measure the drone's stopping distance.

Every run prints its seed and parameters; failures are listed again in the
summary. `--only-seed` replays a single draw on one instance, which can also
be done against a windowed (non headless) Godot to watch the failure live.

## How a run is judged

- `recovered`: the flight core reached HOVER and held it for 3 simulated
  seconds.
- `cutoff`: the safety cutoff latched (impact, saturation, lost attitude).
- `timeout`: 12 simulated seconds elapsed without a stable hover; the summary
  shows the furthest phase reached (a throw too weak for the detector stays
  in `armed`, by design).
- `setup-failed` / `stalled`: the harness itself misbehaved (arming not
  acknowledged before the throw, a `--set` refused or unanswered, the plant
  never acknowledged the scenario); these are tooling problems, not flight
  results.
- `degraded link`: the lockstep link lost a tick during the run. The
  trajectory produced is not the one the scenario asked for, so the run is
  not a flight result either.

A campaign exits nonzero when the recovery rate falls under `--min-recovery`,
and also when any run stalled, failed setup or ran over a degraded link: a
campaign that produced those measured nothing.

## Reproducibility

The flight process hashes the trajectory of every run - relative timestamp,
gyro, accelerometer, barometer and motors of every stepped frame - and
broadcasts the sealed hash on the telemetry port. It lands in the `traj_hash`
CSV column and on every result line.

The promise: **the same scenario played twice in the same plant produces the
same hash.** `--verify-repro` checks exactly that, playing one seed twice
back to back in a single instance and comparing:

```sh
python3 tools/batch/run_batch.py --verify-repro --seed 7
```

It exits nonzero when the two hashes differ, when either run failed to seal
one, or when the link degraded during the check.

Cross-machine caveat: hashes are only comparable **within one campaign**, on
one machine and one Godot build. Jolt does not guarantee bit-identical
physics across machines or versions, so a hash is never committed as an
expected value - it is a witness that two runs of one campaign were the same
run, not a fingerprint of the flight core.

Run i uses seed `--seed + i`, so a campaign is reproducible on the same
machine and Godot build; rates are comparable across machines, individual
trajectories may not be.
