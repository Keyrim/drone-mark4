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
./software/build/desktop/hub/hub
```

The hub takes **no arguments**. It decodes, records and serves with its
built-in defaults (endpoint on 127.0.0.1:47810, announce 47806, telemetry
47801, sim raw 47802, recordings in `logs/` (blackbox files in `logs/blackbox/`, stream CSV pairs in `logs/streams/`), profiles in `profiles/`, pages
in `software/hub/pages/dist` resolved from the binary location); it watches the
default ports from the start and follows any extra telemetry port a process
announces for as long as that process lives. Everything operational is
driven at runtime through the websocket by the pages: opening the board
UART (`serial` message, Add drone block of the control page), toggling the
stream recording (`record` message; a CSV session opens at startup),
replaying a blackbox (`replay` message), and the tuning profiles. A default
worth changing is a compile-time change in `protocol/ports.hpp` or
`HubApp::Config`, not a flag.

The hub never starts Godot or a flight process: both are yours to run and
restart at will (Godot from its own terminal or the "godot sim" VS Code
task, `drone_sim` from anywhere). The plant idles and resends until
the sim port answers, discovery picks each incarnation up within a second,
so the hub is the process that stays up for the whole bench session. The
one child the hub ever spawns is `drone_replay`, on a stored blackbox, for
the re-execute feature.

## Pages

`GET /` serves `index.html` from the pages directory, `GET /<path>` the file
at that path below it. The hub resolves `software/hub/pages/dist` from its own
location, falling back to that relative path. A missing directory is one log
line at startup and a 404 per request, never a startup failure.

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
   "commandPort":47805,"viaSerial":false,"ageMs":120}],
 "bridges":[
  {"address":"192.168.1.31","port":47830,"name":"c19f6c",
   "device":"udp:192.168.1.31:47830","ageMs":220}]}

{"type":"status","recording":false,"serialOpen":true,
 "serialLink":"udp:192.168.1.31:47830",
 "counts":{"telemetryRows":0,"simRawRows":0,"blackboxRecords":0,
           "badFrames":0,"rejectedAnnounces":0},"clients":1,"rcClients":0,
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

{"type":"ota","phase":"transfer","verdict":"none","verdictText":"",
 "lastError":"","autoConfirm":true,"confirmReady":false,"targetSlot":1,
 "bundle":{"loaded":true,
   "path":"software/build/stm32/drone_firmware/drone_firmware.ota",
   "name":"drone_firmware","mcuId":1,"version":"1.3.0","gitHash":"bbbbbbbb",
   "protocolVersion":12,"images":[{"slot":0,"size":8512,"crc32":111},
                                  {"slot":1,"size":8512,"crc32":222}]},
 "board":{"seen":true,"mcuId":1,"runningSlot":0,"slotState":[3,255],
   "slotStateNames":["valid","empty"],"updaterBusy":false,"version":"1.2.0",
   "gitHash":"aaaaaaaa","slotSize":393216,"maxChunkData":240},
 "progress":{"sentBytes":3840,"ackedBytes":1920,"totalBytes":8512,
             "retries":0,"percent":22.5}}
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
disappears, and whenever the set of bridges changes.

`bridges` are the WiFi bridges heard on udp/47831, which they announce
themselves to once a second (see `esp32-bridge/`). They are not processes and
carry no telemetry of their own: a bridge is an address a `serial` link can be
opened on, which is why the entry hands back the exact `device` string to open.
Nobody chooses that address - a router does - so this is what spares the
operator from having to know it. A bridge silent for three seconds is dropped.
`serialLink` in `status` is the device the link is open on, so a page can tell
which of the bridges it is talking to.

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
{"type":"replay","id":18,"name":"board_20260807_150143.m4bb","speed":"max"}
{"type":"serial","id":19,"action":"open","device":"/dev/ttyUSB0","baud":921600}
{"type":"serial","id":20,"action":"close"}
{"type":"otaStatus","id":21}
{"type":"otaStart","id":22,"bundle":"software/build/stm32/drone_firmware/drone_firmware.ota"}
{"type":"otaAbort","id":23}
{"type":"otaConfirm","id":24}
{"type":"otaRevert","id":25}
{"type":"otaConfig","id":26,"autoConfirm":false}
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

A `replay` starts a `drone_replay` next to the hub on one blackbox recording
of the log directory, addressed by the exact `name` `/api/recordings` gave
it: a replay session without leaving the page. The child announces itself, so
discovery names it and its telemetry joins the usual stream; the `ack` says
it started, not that it finished. `speed` is optional, `"max"` or a positive
number as a string, and nothing else ever reaches the command line. One
replay at a time: starting a second one ends the first, because both would
broadcast on the same telemetry port.

Routing: an RC message for `firmware` goes out serial-framed on the UART; an
RC message for any other kind goes to the command port that process
announced. A scenario is routed the same way, to the flight process driving
the plant - no port is hardwired. A reboot needs the board.

A message carrying an `id` is answered with an `ack`, including when it
failed to decode. Streams are never acknowledged.

## Firmware update

An `otaStart` sends one `.ota` bundle to the board over the link that is
already open: the updater packets of `protocol/ota.hpp` are one more packet
type on the framed serial stream, so telemetry keeps flowing between them and
the ESP32 bridge needs to know nothing about any of it.

The `bundle` field is optional and defaults to
`software/build/stm32/drone_firmware/drone_firmware.ota`, resolved from the
hub binary: the common case is one click after a build. Loading validates the
bundle against itself (magic, protocol version, announced sizes and CRC-32,
and each image header against the manifest entry describing it) and then
against the board (right chip, an image for the inactive slot, an image that
fits a slot). Only then does a byte go out.

The session then walks one phase at a time, and every change is published as
one `ota` message, which is what makes a progress bar move without anybody
polling:

```
idle -> query -> erasing -> transfer -> verifying -> rebooting
     -> waitingBoard -> testing -> confirmed
```

The transfer is go-back-N: chunks of at most 240 bytes at strictly increasing
offsets, at most 16 in flight, one cumulative `nextOffset` acknowledgement per
window. A 500 ms acknowledgement silence resends from the last acknowledged
offset; a bounded number of those and the session fails with the offset it
died at. Chunks are paced 2 ms apart, because the hub reaches the board over
WiFi and the board over a 921600 baud UART, and the poll loop tightens to 1 ms
for the duration so that pacing is the throttle rather than the sleep.

`progress.ackedBytes` is what the board has written and is what a bar must
show; `sentBytes` runs up to one window ahead of it and goes backwards on a
resend.

After the reboot the hub polls `OTA_STATUS_REQUEST` once a second until the
board answers again, ignoring answers for the first 1.5 s (the old image can
still answer one request between the command and the reset). What comes back
decides the verdict, and the verdict is a sentence in `verdictText`:

- the bundle's git hash, on a slot reported `testing`: the trial boot worked.
  With `autoConfirm` on, the hub sends `OTA_CONFIRM` once the new image has
  answered at least three status requests over at least three seconds; with it
  off, the operator's `otaConfirm` does. Either way the answer moves the phase
  to `confirmed`.
- the git hash it ran before: the bootloader rolled back. Phase `rolledBack`,
  and nothing is confirmed.
- neither: phase `failed`, saying what it found.

`otaRevert` asks the board to activate its other slot and reboots it; it is
legal while a trial image runs, which is exactly when it is wanted.
`otaAbort` drops the session and tells the board so its half-written slot is
released now rather than at its own timeout. Every refusal the board sends
(`DENIED_ARMED`, `CRC_MISMATCH`, ...) comes back as the sentence behind the
code, never as the code.

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
- A `replay` message next to a live flight process starts a second
  broadcaster on the telemetry port; naming the source in each page is what
  keeps that readable.
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
