# hub

The single process that decodes the binary protocol on behalf of humans.

Everything else in the system speaks `protocol/` over UDP (or over the board
UART, framed). The hub is the one place those bytes become JSON: it watches
the announce broadcast to find out who is alive, follows the telemetry ports
those processes serve, records the streams to disk, and publishes everything
on one websocket endpoint that a browser or a script can read without ever
touching a socket or a packed struct. Commands travel the other way through
the same endpoint.

It links `protocol/` headers and nothing else: never `flight-core`, never
`platform`. Desktop only.

## Building and running

```sh
cmake --preset desktop && cmake --build --preset desktop
./build/desktop/apps/hub/hub serve
```

## Subcommands

### `hub serve [options]`

Decode, record and serve. Watches the announce port, the default telemetry
port and the default sim raw port from the start, and follows any extra
telemetry port a process announces for as long as that process lives.

```
--ws-port N          websocket endpoint port (default 47810)
--announce-port N    announce listen port (default 47806)
--telemetry-port N   telemetry port watched by default (default 47801)
--raw-port N         sim raw port watched by default (default 47802)
--sim-command-port N port scenario commands are sent to (default 47804)
--serial DEV         board UART to own, none by default
--baud N             board UART speed (default 921600)
--record             open a CSV recording at startup
--log-dir DIR        directory recordings are written to (default logs)
```

### `hub up sim [options]`

Starts the Godot simulator and the flight process, then serves next to them.
The Godot project is imported first if it has never been (a fresh checkout).
Godot is started before the flight process: it resends until the flight
process answers, while the flight process would give up waiting for a slow
Godot boot. When any child exits, the hub stops, takes the whole group down
and exits with that child's code. One Ctrl-C ends the scenario.

```
--godot PATH       godot 4 binary (default godot)
--drone-sim PATH   drone_sim binary (default: next to the hub binary)
--headless         run godot without a window
--lockstep         run the simulator in lockstep with the flight process
--time-scale F     simulator speed factor
--arena-radius F   circular wall around the launch point [m]
--frames N         frame budget of the flight process
--sim-port N       sim link port (default 47800)
--command-port N   scenario command port (default 47804)
--rc-port N        rc uplink port (default 47805)
--no-serve         supervise the children only, serve nothing
```

Every `serve` option is accepted too.

### `hub up real [options]`

Owns the board UART and serves. The link is required: the hub refuses to
start when the port cannot be opened. Default device `/dev/ttyUSB0`.

### `hub up replay <log.m4bb> [options]`

Replays a recording next to the hub.

```
--speed F|max        replay speed
--drone-replay PATH  drone_replay binary (default: next to the hub binary)
--no-serve           supervise the child only, serve nothing
```

## Websocket messages

One JSON object per message, keys named exactly like the fields of the wire
structs in `protocol/`.

### Published by the hub

```json
{"type":"telemetry","sourceId":2,"sequence":7,"timestampUs":1234,
 "gyroRadS":[0,0,0],"attitudeQuat":[1,0,0,0],"gyroBiasRadS":[0,0,0],
 "motor":[0,0,0,0],"altitudeM":0.0,"verticalVelocityMps":0.0,
 "throwState":0,"throwCount":0,"releaseVelocityMps":0.0,
 "apexTimestampUs":0,"apexAltitudeM":0.0,"flightPhase":0}

{"type":"simRaw","sourceId":4,"sequence":7,"timestampUs":1234,
 "attitudeQuat":[1,0,0,0],"positionM":[0,0,0],"velocityMps":[0,0,0]}

{"type":"discovery","processes":[
  {"kind":2,"kindName":"drone_sim","sessionId":12345,"telemetryPort":47801,
   "commandPort":47804,"viaSerial":false,"ageMs":120}]}

{"type":"status","recording":false,"serialOpen":true,
 "counts":{"telemetryRows":0,"simRawRows":0,"blackboxRecords":0,
           "badFrames":0,"rejectedAnnounces":0},"clients":1}

{"type":"ack","id":7,"ok":true,"error":""}
```

A client that connects gets a `discovery` and a `status` message
immediately; `status` is then republished once per second and whenever the
recording is toggled, `discovery` whenever a process appears, restarts or
disappears.

### Sent by a client

```json
{"type":"rc","id":7,"target":"firmware","kill":0,"arm":1,"mode":0,"throttle":0.5}
{"type":"simCommand","id":8,"command":"reset"}
{"type":"simCommand","command":"throw","velocityMps":[0,0,6],"angularVelocityRadS":[0,0,0]}
{"type":"simCommand","command":"handThrow","velocityMps":[0,0,6],
 "angularVelocityRadS":[0,0,0],"heldSeconds":1.5,"heldTiltRad":0.3,
 "heldAzimuthRad":0.0,"swingSeconds":0.35}
{"type":"reboot","id":9,"target":"firmware"}
{"type":"record","id":10,"action":"start"}
```

`target` is a process kind name: `firmware`, `drone_sim`, `drone_replay`,
`sim_plant`. `kill`, `arm` and `mode` are integers, `throttle` a number in
[0, 1]. Every field but `type` (and `command` / `action`) is optional and
defaults to zero.

Routing: an RC message for `firmware` goes out serial-framed on the UART; an
RC message for any other kind goes to the command port that process
announced. Scenario commands go to the configured simulator command port. A
reboot needs the board.

A message carrying an `id` is answered with an `ack`, including when it
failed to decode. Streams are never acknowledged.

## Recording

`--record`, or a `record` message, opens a timestamped CSV pair in the log
directory: `streams_YYYYmmdd_HHMMSS_telemetry.csv` and
`..._simraw.csv`. Those files are byte-compatible with what
`tools/blackbox/stream_record.py` writes, down to the header line, the CRLF
terminator and the decimal rendering of every value, so
`tools/blackbox/stream_compare.py` reads a recording made by either.

Blackbox records arriving over the serial link are appended verbatim to
`board_YYYYmmdd_HHMMSS.m4bb`, which is created only once a record actually
arrives.

## Known limitations

- An RC message aimed at a simulator is refused with `no process of kind
  drone_sim` until such a process announces itself with a command port and
  accepts `RcCommandPacket` there.
- Acks are broadcast to every connected client rather than sent back to the
  one that asked: a client correlates the answer with the `id` it sent and
  ignores the rest. This keeps the endpoint free of any per-client state
  shared between the library threads and the poll loop.
- The websocket endpoint has no authentication and binds every interface.
  It is a bench tool on a trusted network.
- With the serial rebroadcast on, the hub ignores firmware telemetry
  arriving over UDP: that copy is its own echo of what it just re-emitted.
  A second, genuinely different board reaching the hub over UDP while a
  first one is wired to it would therefore be invisible.
- POSIX only (`/proc/self/exe`, `posix_spawn`, `termios`, `poll`).
