# Monte Carlo throw campaigns

`run_batch.py` measures the recovery rate of the flight core over randomized
throws, using the real Godot physics as the single reference: it spawns N
(godot headless, drone_sim) pairs in lockstep, resets the world before every
run, plays a randomized throw through the sim command channel and judges the
outcome from the telemetry.

## Requirements

- `drone_sim` built for the desktop preset.
- A Godot 4 binary able to open `sim-godot/` (same version as the editor).
  Headless needs no GPU: a Linux build works inside WSL or a container.
  Pass it with `--godot /path/to/godot` if it is not on the PATH.

The first run on a fresh checkout imports the Godot project automatically.

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
```

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
  acknowledged, telemetry silent); these are tooling problems, not flight
  results.

Reproducibility note: run i uses seed `--seed + i`, so a campaign is
reproducible on the same machine and Godot build. Jolt does not guarantee
bit-identical physics across machines; rates are comparable, individual
trajectories may not be.
