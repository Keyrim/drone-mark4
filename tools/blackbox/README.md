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
