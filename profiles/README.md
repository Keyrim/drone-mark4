# Tuning profiles

Named sets of tuning values, one JSON file per profile, tracked in git.

Nothing on the flight side stores tuning: neither the board nor the
simulator has anywhere to keep it, so both boot on the compiled-in defaults
every single time. This directory is where a bench session that found a good
set writes it down, and where the hub reads it back from to push it again.

## Format

`<name>.json`, the name being 1 to 32 characters of `[A-Za-z0-9_-]` (it is a
file name, so the accepted set is deliberately narrow).

```json
{
  "version": 1,
  "values": {
    "101": 0.028,
    "303": 0.55
  }
}
```

`values` maps a parameter id to its value. The ids are the ones in
`flight-core/include/flight_core/tuning_table.hpp`, written as decimal
strings because JSON object keys are strings. An id that no longer exists is
answered with `unknownId` when pushed, and costs nothing else.

Only `version` 1 is read. A file that fails to parse is refused with a
reason and never takes the hub down with it.

## Using them

```sh
hub profile list                 # names in this directory
hub profile show bench           # one profile, as JSON
hub serve --push-profile bench   # push it to every process that announces
hub serve --profiles /other/dir  # read profiles from elsewhere
```

Over the websocket: `profileList`, `profileSave`, `profileLoad` and
`profilePush`. See `apps/hub/README.md`.

No profile ships with the repo: what belongs here is what a real bench
session measured, and a made-up example would only invite someone to fly it.
