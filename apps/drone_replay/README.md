# drone_replay

Replays a blackbox file through the flight core in open loop and
re-broadcasts telemetry over UDP, so the usual listeners (ground station,
Godot viewer) can watch a past run. Desktop preset only.

```sh
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb              # real time
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb --speed 0.1  # slow motion
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb --speed max  # as fast as possible
```

It announces itself once per second of real time, like every other flight
process, so the hub discovers it and a page can address it by name. The
announce carries the telemetry port and no command port: nothing steers a
replay. `--announce-port N` moves the broadcast off the default port, for a
hub started on one of its own.
