# Sim stub

Stand-in for a physics simulator. `sim_stub.py` streams synthetic sensor
packets over UDP to the flight process (`drone_sim` listens on
127.0.0.1:47800 by default) and listens on the same socket for the actuator
packets the flight process sends back to the sender address. Frame content is
a gyro sine, a specific force at rest, a fixed static pressure, kill switch
released and a constant throttle. Simulated time comes from the frame counter
and the requested rate, so the stream is deterministic; the loop is paced
against absolute wall-clock deadlines so the rate does not drift.

Python 3 standard library only, no dependencies.

## Usage

```sh
# Default run: 10 s at 1000 frames per second towards 127.0.0.1:47800
./sim_stub.py

# Run until Ctrl-C
./sim_stub.py --duration 0

# Custom rate, destination and gyro excitation
./sim_stub.py --host 127.0.0.1 --port 47800 --rate 250 --amplitude 1.5 --frequency 5
```

A status line is printed roughly once per second (frames sent, replies
received, last motor values), plus a summary on exit. The stub exits with
code 0 on Ctrl-C and when the duration elapses. Running with no receiver is
fine: the ICMP port unreachable that some systems report on a later send is
ignored and streaming continues.

## Packet formats

Source of truth: `software/components/protocol/include/protocol/sim_link.hpp`, mirrored by the
shared `tools/telemetry_wire.py` module the stub imports.
Both packets are little-endian and packed, version byte then type byte:
sensor packet 45 bytes, actuator packet 84 bytes. Datagrams with the
wrong size, version or type are ignored.

The sensor packet carries sensors only. The stub therefore streams no
pilot state at all, and the flight process it feeds stays in its RC
fail-safe (kill engaged): the stub is a sensor source, not a cockpit.
The scenario block trailing the motors in every actuator packet is ignored
for the same reason: the stub has no world to reset.
