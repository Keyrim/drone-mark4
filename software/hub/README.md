# hub

The single process that decodes the binary protocol on behalf of humans.

Everything else in the system speaks the wire of
`software/components/protocol/mark4.proto`, one `Envelope` per transport
frame (`software/components/transport/`): the desktop flight process over
UDP, the board over its UART, which the WiFi bridge carries inside UDP
datagrams. The hub is the one place those bytes become JSON: it is one
transport node (kind `gateway`, it beacons like the others) with two links,
the LAN and the bridge, and it relays between them so the board and the LAN
nodes see each other. It learns who is alive from the `Announce` the other
nodes send, and publishes everything on one websocket endpoint that a
browser or a script can read without ever touching a socket or a codec.
Commands travel the other way through the same endpoint.

That same TCP port also serves the static pages: the library dispatches on
the `Upgrade` header, so a page loaded from the hub reaches it back with
`new WebSocket("ws://" + location.host)` and never learns a port of its own.

It links the `protocol` and `transport` libraries and nothing else: never
`flight-core`, never `platform`. Desktop only.

## Building and running

```sh
cmake --preset desktop && cmake --build --preset desktop
./software/build/desktop/hub/hub
```

The hub takes **no arguments**. It decodes and serves with its built-in
defaults (endpoint on 127.0.0.1:47810, transport discovery port 47820,
bridge announces 47831, profiles in `profiles/`, pages in
`software/hub/pages/dist` resolved from the binary location). A flight
process reaches it by beaconing on the discovery port; nothing is wired by
hand. Everything operational is driven at runtime through the websocket by
the pages: connecting to a drone (`connect` message, Connections panel of
the control page) and the tuning profiles. A default worth changing is a
compile-time change in `protocol/ports.hpp`, `transport/udp_link.hpp` or
`HubApp::Config`, not a flag.

One drone at a time is THE connected drone. Everything the hub hears is
still decoded and published to the clients - being connected
decides where the commands go (RC, tuning, scenario, reboot, update) and
which drone the control page pilots; a command aimed anywhere else is
refused. The connection is held by the hub, so every tab sees the same
drone, and it survives losing that drone: silence flips `live` to false and
keeps the target, the same drone coming back flips it true again on its
own. Only a `disconnect` (or a `connect` elsewhere) lets go. The identity
the reconnection works on is the route: a UDP process is its kind, a board
is the bridge it is reached through, by name (the bridge rides the drone, so
its name is the drone's; a bridge whose address changes is followed by
name).

The hub never starts Godot or a flight process: both are yours to run and
restart at will (Godot from its own terminal or the "godot sim" VS Code
task, `drone_sim` from anywhere). The plant idles and resends until
the sim port answers, discovery picks each incarnation up within a second,
so the hub is the process that stays up for the whole bench session.

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
  {"kind":2,"kindName":"drone_sim","sessionId":12345,"ageMs":120}],
 "bridges":[
  {"address":"192.168.1.31","port":47830,"name":"c19f6c","ageMs":220}]}

{"type":"status",
 "connection":{"via":"bridge","id":"c19f6c","kind":1,"kindName":"firmware",
               "live":true},
 "counts":{"telemetryRows":0,"simRawRows":0,
           "badFrames":0,"rejectedAnnounces":0},"clients":1,"rcClients":0,
 "links":[{"stream":"telemetry","sourceId":2,"sourceName":"drone_sim",
           "received":100,"lost":0,"duplicates":0,"resyncs":0,
           "lossRate":0.0,"lastSequence":99}]}

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

`links` holds one entry per (stream, source) pair the hub has seen, read
from the sequence number every stream packet carries. The number is 16 bits
and wraps, so the distance between two packets is read in that arithmetic; a
forward jump of more than 1024 is counted as one `resyncs` rather than as a
thousand losses, because that is a sender restarting or the hub joining a
stream already in flight. `sourceName` is what discovery calls that source,
empty while nobody has announced it. `lossRate` is `lost / (received +
lost)`. A `transport` entry per process reached over the transport carries
the frame counters of its node (every payload type included), as the
transport keeps them.

`sessionId` in `discovery` is the transport node id of the process: a
process draws a new one every start, so a change behind the same kind is a
restart. The board's is derived from its MCU unique id and never changes.

`statusName` is one of `ok`, `unknownId`, `outOfBounds`, `lockedWhileArmed`.
`source` is the kind the sending node announced itself as. Tuning answers
share the telemetry stream and carry no source byte of their own.

```json
{"type":"log","source":"firmware","timestampUs":1234,"level":1,
 "text":"baro: init failed, flying without the pressure channel"}
```

`log` is one console line of a node (`Log` envelope; level 0 info, 1
warn, 2 error). The firmware sends its init failures and update state
changes this way, so a board updated over the air with no probe attached
still says what happened; the hub also prints the line on its stdout and
the pages show it as a toast.

A client that connects gets a `discovery` and a `status` message
immediately; `status` is then republished once per second and whenever the
connection changes (target or liveness), `discovery` whenever a process
appears, restarts or disappears,
and whenever the set of bridges changes.

`connection` in `status` is THE connected drone: `via` is `none`, `udp` or
`bridge`, `id` the identity on that route (kind name or bridge name),
`kind`/`kindName` where commands route, and `live` whether
the drone currently shows signs of life. A lost drone keeps its entry with
`live` false until a `disconnect`.

`bridges` are the WiFi bridges heard on udp/47831, which they announce
themselves to once a second (see `esp32-bridge/`). They are not processes
and carry no telemetry of their own: a bridge is a door a board is connected
through, by name (`connect` with `via":"bridge"`). Nobody chooses its
address - a router does - so nothing is typed: the hub resolves the name to
today's address, opens its bridge link (a `UartLink` over a UDP socket to
that address) and follows it if the router hands out another one. The
hub's own beacon is the first datagram the bridge receives, which is how
the bridge learns where to send the board's bytes: there is no hello. The
board then shows up in `processes` like any node, kind `firmware`, and
`connection.live` follows its Announce. A bridge silent for three seconds
is dropped from `bridges`, which does not drop a connection through it; a
`disconnect` closes the bridge link.

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
{"type":"tuningSet","id":11,"target":"drone_sim","paramId":101,"value":0.028}
{"type":"tuningList","id":13,"target":"drone_sim","startIndex":0}
{"type":"profileList","id":14}
{"type":"profileSave","id":15,"name":"bench","values":{"101":0.028}}
{"type":"profileLoad","id":16,"name":"bench"}
{"type":"profilePush","id":17,"name":"bench","target":"drone_sim"}
{"type":"connect","id":19,"via":"udp","target":"drone_sim"}
{"type":"connect","id":19,"via":"bridge","name":"c19f6c"}
{"type":"disconnect","id":20}
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

`target` is a process kind name: `firmware`, `drone_sim`, `sim_plant`. `kill`, `arm` and `mode` are integers, `throttle` a number in
[0, 1]. Every field but `type` (and `scenario` / `action`) is optional and
defaults to zero; a `simScenario` defaults its `target` to `drone_sim`.

One `simScenario` message is one whole run: it opens with a reset and the
plant schedules everything else from that reset tick. `seed` seeds every
generator of the run, `throwDelayUs` places the throw after the reset, and
`hashWindowUs` sets how much of the run the flight process hashes. The
`sequence` byte is what makes a scenario idempotent - leave it out and the
hub stamps a rolling one, so two scenarios in a row are two runs.

Routing: a command only goes out when its `target` is the connected drone
(the `ack` says `no drone connected` or `connected to X, not to Y`
otherwise). Every command is a transport unicast to the node whose beacon
announced that kind, the board included: the transport knows it was heard
on the bridge link and frames it for the UART. A scenario is routed the
same way, to the flight process driving the plant - no port is hardwired. A
reboot needs the board. `otaAbort` is the one exception to the gate:
dropping a stuck transfer must work even after the drone is gone.

A message carrying an `id` is answered with an `ack`, including when it
failed to decode. Streams are never acknowledged.

## Firmware update

An `otaStart` sends one `.ota` bundle to the board over the transport, as
unicasts to its node: the updater messages are one more body of the same
envelope on the same link, so telemetry keeps flowing between them and the
ESP32 bridge needs to know nothing about any of it.

The `bundle` field is optional and defaults to
`software/build/stm32/drone_firmware/drone_firmware.ota`, resolved from the
hub binary: the common case is one click after a build. Loading validates the
bundle against itself (magic, wire hash, announced sizes and CRC-32,
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

After the reboot the hub polls `OtaStatusRequest` once a second until the
board answers again, ignoring answers for the first 1.5 s (the old image can
still answer one request between the command and the reset). What comes back
decides the verdict, and the verdict is a sentence in `verdictText`:

- the bundle's build epoch, on a slot reported `testing`: the trial boot
  worked. The image confirms itself on the first request it serves, so the
  hub keeps polling until the slot reads `valid`, which moves the phase to
  `confirmed`; nothing is sent to confirm.
- the git hash it ran before: the bootloader rolled back. Phase `rolledBack`,
  and nothing is confirmed.
- neither: phase `failed`, saying what it found.

`otaRevert` asks the board to activate its other slot and reboots it; it is
legal while a trial image runs, which is exactly when it is wanted.
`otaAbort` drops the session and tells the board so its half-written slot is
released now rather than at its own timeout. Every refusal the board sends
(`DENIED_ARMED`, `CRC_MISMATCH`, ...) comes back as the sentence behind the
code, never as the code.

## Known limitations

- An RC message aimed at a kind no process has announced is refused with
  `no process of kind <kind>`; a scenario has to be up first.
- Acks are broadcast to every connected client rather than sent back to the
  one that asked: a client correlates the answer with the `id` it sent and
  ignores the rest. This keeps the endpoint free of any per-client state
  shared between the library threads and the poll loop.
- The endpoint has no authentication. It binds the loopback interface, and
  it is a bench tool on a trusted network.
- The hub relays every broadcast between the LAN and the bridge link: the
  board hears the LAN's telemetry too (a few KB/s on its UART, ignored by
  its command path) and the LAN hears the board's. Two hubs on one LAN
  would each relay what the other relays; the hop count and the duplicate
  drop bound it, nothing forbids it.
- Tuned values do not survive a simulator reset: `drone_sim` rebuilds its
  flight core on the reset (there is no state a teleport could keep) and
  does not re-announce, so the hub has no event to push a profile on. Push
  it again explicitly with `profilePush` after resetting the world.
- POSIX only (`/proc/self/exe`, `posix_spawn`, `poll`).
