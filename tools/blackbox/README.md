# Blackbox tools

`blackbox_dump.py` decodes the `.m4bb` files drone_sim writes into `logs/`
(one timestamped file per run). Python stdlib only.

```sh
python3 tools/blackbox/blackbox_dump.py               # summary of the newest log
python3 tools/blackbox/blackbox_dump.py --csv         # full CSV on stdout
python3 tools/blackbox/blackbox_dump.py logs/x.m4bb   # a specific file
```

The record layout is defined by `protocol/blackbox.hpp` (self-framing:
sync marker, version, type, length, CRC-16); the decoder comes from the
shared `tools/ground-station/telemetry_wire.py` module and resynchronizes
on the sync marker, so a torn record costs only itself.

`stream_record.py` captures the telemetry (udp/47801) and sim raw (udp/47802)
broadcasts of a live session into a timestamped CSV pair under `logs/`; it
waits for the first packet and stops when the streams go idle.
`stream_compare.py` then scores the estimated state against the exact one
(RMS / max errors, worst one-second windows) - by default on the newest pair:

```sh
python3 tools/blackbox/stream_record.py    # leave running during a session
python3 tools/blackbox/stream_compare.py   # score the newest recording
```
