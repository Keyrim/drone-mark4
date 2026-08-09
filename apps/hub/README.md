# hub

The single process that decodes the binary protocol on behalf of humans.

Everything else in the system speaks `protocol/` over UDP (or over the board
UART, framed). The hub is the one place those bytes become JSON: it watches
the announce broadcast to find out who is alive, follows the telemetry ports
those processes serve, records the streams to disk, and publishes everything
on one websocket endpoint that a browser or a script can read without ever
touching a socket or a packed struct. Commands travel the other way through
the same endpoint.

That same TCP port also serves the static pages: the library dispatches on
the `Upgrade` header, so a page loaded from the hub reaches it back with
`new WebSocket("ws://" + location.host)` and never learns a port of its own.

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
--ws-port N          endpoint port, websocket and pages (default 47810)
--bind ADDR          address the endpoint binds to (default 127.0.0.1)
--pages DIR          directory the static pages are read from
--announce-port N    announce listen port (default 47806)
--telemetry-port N   telemetry port watched by default (default 47801)
--raw-port N         sim raw port watched by default (default 47802)
--serial DEV         board UART to own, none by default
--baud N             board UART speed (default 921600)
--record             open a CSV recording at startup
--log-dir DIR        directory recordings are written to (default logs)
--profiles PATH      directory tuning profiles live in (default profiles)
--push-profile NAME  push this profile to every process that appears
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
--rc-port N        rc uplink port (default 47805)
--no-serve         supervise the children only, serve nothing
```

Every `serve` option is accepted too.

### `hub profile list` / `hub profile show NAME`

Plain file reads in the profiles directory: no socket, no running hub. See
`profiles/README.md` for the file format.

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

## Pages

`GET /` serves `index.html` from the pages directory, `GET /<path>` the file
at that path below it. `--pages` names the directory; without it the hub
resolves `apps/hub/pages/dist` from its own location, falling back to that
relative path. A missing directory is one log line at startup and a 404 per
request, never a startup failure.

The Content-Type comes from the extension (`.html`, `.js`, `.mjs`, `.css`,
`.svg`, `.json`, `.csv`, `.ico`, `.png`; anything else is an opaque byte
stream), and every response carries `Cache-Control: no-store`. A URI holding
a `..` component is refused: nothing outside the pages directory is
reachable.

## HTTP API

`/api/` is the recordings, and only the recordings: files on disk, finished
business, safe to read from any connection thread. Everything live is a
websocket message. A handler here never touches the recorder, the discovery
table or the counters, which is why the hub holds no lock.

A recording is addressed by the exact `name` the listing gave it. That
listing is the whole address space: a name it does not hold addresses
nothing, so nothing a caller sends is ever turned into a path. An error is
`{"error":"..."}` with the matching status.

```
GET /api/recordings
  -> {"logDir":"logs","recordings":[
       {"name":"board_20260807_150143.m4bb","kind":"blackbox","sizeBytes":N,
        "modifiedUnixS":N,"estimatedRecords":N},
       {"name":"streams_20260805_225701","kind":"streams","sizeBytes":N,
        "modifiedUnixS":N,"telemetryFile":"..._telemetry.csv",
        "simRawFile":"..._simraw.csv"}]}

GET /api/recording?name=X[&from=&to=&maxPoints=]
  streams   -> {"name","kind":"streams","window":{"fromUs":..,"toUs":..},
                "telemetry":{"total":N,"stride":N,"count":N,
                             "columns":[...],"rows":[[...]]},
                "simRaw":{...}}
  blackbox  -> {"name","kind":"blackbox","total":N,"stride":N,"count":N,
                "skippedBytes":N,"columns":[...],"rows":[[...]]}

GET /api/compare?name=X[&from=&to=&maxPoints=]        (streams only)
  -> {"maxGapUs":30000,"alignedSamples":N,"unmatched":N,"durationS":F,
      "metrics":[{"name":"attitude","unit":"deg","rms":F,"max":F,
                  "worstWindows":[{"startS":F,"rms":F}]}, ...],
      "series":{"total":N,"stride":N,"count":N,
                "columns":["timestamp_us","attitude_deg","altitude_m","vz_mps"],
                "rows":[[...]]}}

GET /api/summary?name=X                               (blackbox only)
  -> {"records":N,"durationS":F,"rateHz":F,
      "accelNormG":{"min":F,"max":F},"killRecords":N,"skippedBytes":N}

GET /api/file?name=X[&part=telemetry|simraw|raw|csv]
  -> the file itself, as an attachment
```

Most recent first; `simRawFile` is empty when the pair has no exact half;
`batch_*.log` files are not recordings. A streams recording is named by the
prefix its two files share, a blackbox recording by its file name.
`estimatedRecords` is the file size divided by the record size, not a count:
a listing must stay cheap however long the run was.

`columns` are the header line of the recorded CSV, verbatim, or the fields of
a blackbox record; rows are arrays, in that column order. `from` and `to`
are timestamps in microseconds, `maxPoints` defaults to 2000 and is capped at
20000. A decode walks the file twice, once to count and once to emit every
`stride`-th point, so nothing large is ever held whole; the first and the
last point of the window are always among them.

`skippedBytes` counts what framed no record: a blackbox decode resynchronizes
on the record marker after a torn write, and a torn write costs only the
record it tore.

`/api/compare` runs the alignment and the scoring of the live `compare`
message on the recorded pair, so the two agree by construction. `part`
defaults to `telemetry` for a pair and to `raw` for a blackbox file;
`part=csv` renders a blackbox file as one line per record, the columns being
the fields of the record.

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
   "commandPort":47805,"viaSerial":false,"ageMs":120}]}

{"type":"status","recording":false,"serialOpen":true,
 "counts":{"telemetryRows":0,"simRawRows":0,"blackboxRecords":0,
           "badFrames":0,"rejectedAnnounces":0},"clients":1,
 "links":[{"stream":"telemetry","sourceId":2,"sourceName":"drone_sim",
           "received":100,"lost":0,"duplicates":0,"resyncs":0,
           "lossRate":0.0,"lastSequence":99}]}

{"type":"compare","timestampUs":1234,"gapUs":-200,"attitudeErrorDeg":0.43,
 "altitudeErrorM":0.12,"verticalVelocityErrorMps":-0.05}

{"type":"ack","id":7,"ok":true,"error":""}

{"type":"tuningAck","source":"drone_sim","paramId":101,"value":0.028,
 "status":0,"statusName":"ok"}

{"type":"tuningInfo","source":"drone_sim","index":0,"count":12,"paramId":101,
 "name":"rate_kp_rp","value":0.028,"minValue":0.0,"maxValue":0.5,
 "armedChange":true}

{"type":"profiles","names":["bench","field-2"]}

{"type":"profile","name":"bench","values":{"101":0.028}}
```

A `compare` message is published whenever a telemetry sample and the exact
simulator state nearest to it can be joined: same instant within 30 ms,
nearest sample wins, no match means no message. It is the one alignment rule
of the system, and the same code answers `/api/compare` on a recording, so a
number read while a session runs and the same number read afterwards are one
number. The price is that a `compare` trails its `telemetry` by up to 30 ms:
until then a nearer exact state could still arrive.

`links` holds one entry per (stream, source) pair the hub has seen, read
from the sequence number every stream packet carries. The number is 16 bits
and wraps, so the distance between two packets is read in that arithmetic; a
forward jump of more than 1024 is counted as one `resyncs` rather than as a
thousand losses, because that is a sender restarting or the hub joining a
stream already in flight. `sourceName` is what discovery calls that source,
empty while nobody has announced it. `lossRate` is `lost / (received +
lost)`.

`statusName` is one of `ok`, `unknownId`, `outOfBounds`, `lockedWhileArmed`.
`source` is inferred from the path the answer arrived by: the serial link
means the board, a telemetry port means the simulator side. Tuning answers
share the telemetry stream and carry no source byte of their own.

A client that connects gets a `discovery` and a `status` message
immediately; `status` is then republished once per second and whenever the
recording is toggled, `discovery` whenever a process appears, restarts or
disappears.

### Sent by a client

```json
{"type":"rc","id":7,"target":"firmware","kill":0,"arm":1,"mode":0,"throttle":0.5}
{"type":"simScenario","id":8,"scenario":"reset","seed":1234}
{"type":"simScenario","scenario":"throw","seed":1234,"throwDelayUs":2000000,
 "velocityMps":[0,0,6],"angularVelocityRadS":[0,0,0]}
{"type":"simScenario","scenario":"handThrow","seed":1234,"velocityMps":[0,0,6],
 "angularVelocityRadS":[0,0,0],"heldSeconds":1.5,"heldTiltRad":0.3,
 "heldAzimuthRad":0.0,"swingSeconds":0.35}
{"type":"reboot","id":9,"target":"firmware"}
{"type":"record","id":10,"action":"start"}
{"type":"tuningSet","id":11,"target":"drone_sim","paramId":101,"value":0.028}
{"type":"tuningGet","id":12,"target":"drone_sim","paramId":101}
{"type":"tuningList","id":13,"target":"drone_sim","startIndex":0}
{"type":"profileList","id":14}
{"type":"profileSave","id":15,"name":"bench","values":{"101":0.028}}
{"type":"profileLoad","id":16,"name":"bench"}
{"type":"profilePush","id":17,"name":"bench","target":"drone_sim"}
```

The parameter id key is `paramId`, never `id`: `id` is the correlation id
every message may carry. `startIndex` is optional and defaults to 0. The
`ack` to a `tuningList` says the request went out, not that the table
arrived: the descriptions follow as their own `tuningInfo` messages, one per
flight frame as the process unrolls them.

`target` is a process kind name: `firmware`, `drone_sim`, `drone_replay`,
`sim_plant`. `kill`, `arm` and `mode` are integers, `throttle` a number in
[0, 1]. Every field but `type` (and `scenario` / `action`) is optional and
defaults to zero; a `simScenario` defaults its `target` to `drone_sim`.

One `simScenario` message is one whole run: it opens with a reset and the
plant schedules everything else from that reset tick. `seed` seeds every
generator of the run, `throwDelayUs` places the throw after the reset, and
`hashWindowUs` sets how much of the run the flight process hashes. The
`sequence` byte is what makes a scenario idempotent - leave it out and the
hub stamps a rolling one, so two scenarios in a row are two runs.

Routing: an RC message for `firmware` goes out serial-framed on the UART; an
RC message for any other kind goes to the command port that process
announced. A scenario is routed the same way, to the flight process driving
the plant - no port is hardwired. A reboot needs the board.

A message carrying an `id` is answered with an `ack`, including when it
failed to decode. Streams are never acknowledged.

## Recording

`--record`, or a `record` message, opens a timestamped CSV pair in the log
directory: `streams_YYYYmmdd_HHMMSS_telemetry.csv` and
`..._simraw.csv`. Those files are what a python consumer would have written
itself, down to the header line, the CRLF terminator and the decimal
rendering of every value, so a python consumer reads them without knowing
which side produced them.

Blackbox records arriving over the serial link are appended verbatim to
`board_YYYYmmdd_HHMMSS.m4bb`, which is created only once a record actually
arrives.

## Known limitations

- An RC message aimed at a kind no process has announced is refused with
  `no process of kind <kind>`; a scenario has to be up first.
- Every announce carries `sessionId` 0 for now, so a process that restarts
  on the same ports is not seen as a new session.
- Acks are broadcast to every connected client rather than sent back to the
  one that asked: a client correlates the answer with the `id` it sent and
  ignores the rest. This keeps the endpoint free of any per-client state
  shared between the library threads and the poll loop.
- The endpoint has no authentication. It binds the loopback interface by
  default; `--bind` opens it wider, and it is a bench tool on a trusted
  network either way.
- With the serial rebroadcast on, the hub ignores firmware telemetry
  arriving over UDP: that copy is its own echo of what it just re-emitted.
  A second, genuinely different board reaching the hub over UDP while a
  first one is wired to it would therefore be invisible.
- Tuned values do not survive a simulator reset: `drone_sim` rebuilds its
  flight core on the reset (there is no state a teleport could keep) and
  does not re-announce, so the hub has no event to push a profile on. Push
  it again explicitly with `profilePush` after resetting the world.
- POSIX only (`/proc/self/exe`, `posix_spawn`, `termios`, `poll`).
