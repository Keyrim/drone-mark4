# Blackbox tools

`blackbox_dump.py` decodes the `.m4bb` files drone_sim writes into `logs/`
(one timestamped file per run). Python stdlib only.

```sh
python3 tools/blackbox/blackbox_dump.py               # summary of the newest log
python3 tools/blackbox/blackbox_dump.py --csv         # full CSV on stdout
python3 tools/blackbox/blackbox_dump.py logs/x.m4bb   # a specific file
```

The record layout is defined by `flight_core/blackbox.hpp`; the struct format
string here must be kept in sync with it (the version byte is checked).

`stream_record.py` captures the telemetry (udp/47801) and sim raw (udp/47802)
broadcasts of a live session into a timestamped CSV pair under `logs/`; it
waits for the first packet and stops when the streams go idle.
`stream_compare.py` then scores the estimated state against the exact one
(RMS / max errors, worst one-second windows) - by default on the newest pair:

```sh
python3 tools/blackbox/stream_record.py    # leave running during a session
python3 tools/blackbox/stream_compare.py   # score the newest recording
```
