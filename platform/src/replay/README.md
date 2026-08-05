# platform/src/replay

Replay variant of the platform services, desktop preset only. Provides
`SensorSourceReplay`: reads the records of a `.m4bb` blackbox file and
serves them as sensor frames, paced by the recorded timestamps scaled by a
speed factor (or unpaced with `SPEED_MAX`). Open loop: the motor commands
stored in the records are ignored. The record layout comes from
`flight_core/blackbox.hpp`.
