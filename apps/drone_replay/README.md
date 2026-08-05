# drone_replay

Replays a blackbox file through the flight core in open loop and
re-broadcasts telemetry over UDP, so the usual listeners (ground station,
Godot viewer) can watch a past run. Desktop preset only.

```sh
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb              # real time
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb --speed 0.1  # slow motion
./build/desktop/apps/drone_replay/drone_replay logs/<file>.m4bb --speed max  # as fast as possible
```
