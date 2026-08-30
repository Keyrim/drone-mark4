# ESP32 relay

Firmware for the ESP32-C3 SuperMini (IPEX variant) riding the drone, wired
to the flight controller's UART. It is a transport relay
(`software/components/transport/`): one transport node with two links, the
board's UART and the WiFi LAN, forwarding frames between them. It is a node
of the system like any other: it beacons its own `Announce` (kind `relay`,
name `relay-<last three bytes of its MAC>`, mcu `ESP32C3`, the build epoch
and short commit hash of its build, the wire hash), and it logs through the
project's log library. From the LAN's point of view the board and the relay
are two nodes on udp/47820 sharing one address, exactly like `drone_sim` is
one; the hub has no path, port or click specific to either.

## What crosses, and what does not

The relay forwards with the transport's generic rules (split horizon, one
hop less per relay, duplicate drop by `(src, seq)`), plus one outbound
filter on the UART link, `uartFilter()` in `main/relay.cpp`:

- towards the LAN: everything the board emits (telemetry, answers, log
  lines, its Announce), as broadcasts;
- towards the UART: every unicast the transport routes there (the board is
  the only node on that link, so a unicast routed there is for it: RC,
  tuning, updater messages), and the broadcasts whose envelope body is an
  `Announce` (the board learns the LAN nodes from those, this relay
  included). Every other LAN broadcast, `drone_sim`'s telemetry and run
  stats first, stays on the LAN: a 921600 baud line does not carry the
  whole LAN.

The filter judges relayed frames only. What the relay says itself is a
`send`, and a `send` names the links it leaves on (`Transport::send`'s link
mask): its log lines and its module table take the LAN bit alone, its
beacon takes both links.

The announce check reads one byte of the envelope
(`envelopeIsAnnounce()`, `protocol/envelope.hpp`): the Envelope has a single
field, its oneof, so the body's tag opens the bytes. Nothing is decoded on
the relay.

## Composition

`main/bridge_main.c` brings the network up (unchanged: STA on the network
named in `local.cmake`, or the `mark4-bridge` access point after ten seconds
without an address) then hands over to `relayRun()` in `main/relay.cpp`:

- `UartStream`, an `AbsByteStream` over the ESP-IDF UART driver rings
  (`uart_read_bytes` with a zero timeout, `uart_write_bytes` only when the
  transmit ring has room for the whole frame, refused and counted
  otherwise), wrapped in the shared `UartLink` (serial framing
  `A5 5A len16 payload crc16`);
- the shared `UdpLink`, compiled as is against lwIP's BSD socket API
  (shared discovery port 47820 with `SO_REUSEADDR`, ephemeral data socket,
  broadcast to 255.255.255.255 with `SO_BROADCAST`). lwIP has no
  `getifaddrs()`; the relay hands its own address to
  `UdpLink::addLocalHost()` so its broadcasts coming back are dropped as
  echoes;
- `Transport` with node id `hashNodeId()` of the WiFi MAC, `setRelay(true)`,
  the filter, and the `Announce` beacon;
- the log library: the shared `ConsoleSinkPosix` (its console is plain
  stdio) and a `TransportSink` broadcasting every line onto the LAN. The
  clock is `esp_timer_get_time()`. The bring-up in `bridge_main.c` is C and
  speaks through two shims, `bridgeLogInfo()` / `bridgeLogWarn()`, rather
  than being ported to C++: the smaller diff of the two.

The main task polls the transport once per FreeRTOS tick (1 ms,
`CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults`): one frame is one
datagram, no aggregation. Every poll drains both links whole; a 160-byte
telemetry frame takes 1.7 ms on the line, so the added latency is under one
frame time either way. The UART driver keeps 4096 bytes on the way in
(about 44 ms of a full line, what a WiFi stall may cost before bytes are
lost and the framing resynchronizes) and 1024 on the way out.

The shared sources are compiled by the ESP-IDF build straight from
`software/components/`: `transport.cpp`, `uart_link.cpp`,
`posix/udp_link.cpp` and the log library (`module.cpp`, `wire.cpp`,
`posix/console_sink_posix.cpp`) in the `main` component, the nanopb runtime,
`envelope.cpp` and the codec generated from
`software/components/protocol/mark4.proto` in `components/mark4_proto/` (same generator, same pinned nanopb commit, same
`PB_NO_MALLOC PB_BUFFER_ONLY` as the desktop build; the generator needs a
python with `protobuf` and `grpcio-tools`, the component uses the first of
the IDF interpreter and `/usr/bin/python3` that has them). The `main`
component carries the project's warning set (`-Wall -Wextra -Wconversion
-Wdouble-promotion -Werror`) and `-fno-exceptions -fno-rtti`.

## Logs

Four modules, ids from `main/log_modules.hpp` (256 up, the shared code
takes its own from `log/module_ids.hpp`): `app/boot`, `app/wifi`,
`relay/core`, `relay/stats`. Every line goes to the USB Serial/JTAG console
(115200) and, once the transport is up, onto the LAN as a `Log` envelope
any client reads. What a healthy relay prints:

```
00:00:01.712 INFO app/wifi: joined <ssid> as 192.168.1.31   (or: access point mark4-bridge)
00:00:01.760 INFO app/boot: boot: node ff42bd55 relay build 1756512000 953448a1 wire 4f2c81de
00:00:01.760 INFO app/boot: uart 921600 baud, lan data port 60278, discovery port 47820
00:00:02.140 INFO relay/core: node 6c41b2f0 up on the uart              (the board)
00:00:02.900 INFO relay/core: node 1cd5e199 up on the lan at 192.168.1.13:41283   (the hub)
00:00:12.010 INFO relay/core: node 1cd5e199 gone                        (3 s of silence)
```

`relay/stats` is the five-second counters line, at DEBUG so it is off by
default; a client turns it on with a `LogControl.set{relay/stats, DEBUG}`
addressed to the relay's node id (the hub's pages list its modules like any
node's) and it starts arriving as `Log` frames:

```
00:00:20.000 DEBG relay/stats: nodes 2, relayed 1063, filtered 250, dropped 0, uart tx full 0
```

`relayed` counts frames forwarded (one per link for a broadcast), `filtered`
the LAN broadcasts the filter kept off the UART, `dropped` the transport's
drops (unknown destination, hops exhausted), `uart tx full` the frames the
UART ring could not take whole. The timestamps are the module's uptime.

## Network

The network to join is named in `esp32-bridge/local.cmake`, which is untracked
because credentials belong to whoever builds:

```cmake
set(BRIDGE_STA_SSID "my-network")
set(BRIDGE_STA_PASSWORD "my-password")
```

Without that file the relay is an access point (`mark4-bridge`, WPA2,
192.168.4.1, see `bridge_main.c`) and nothing else. Either way the only port
in play is udp/47820, the transport's discovery port; the relay's data
socket takes an ephemeral port that the hub learns from the board's frames.
A station that loses its network keeps asking to join it back, and the
`UdpLink` tries the global broadcast again on every send, so a network that
comes back is used again without a reset.

## Wiring

| ESP32-C3 | Flight controller  |
| -------- | ------------------ |
| GPIO3    | UART TX (board out)|
| GPIO4    | UART RX (board in) |
| GND      | GND                |
| 5V       | UART connector +5  |

921600 baud, 8N1, no flow control. The console stays on the USB Serial/JTAG
port of the module, which is why `sdkconfig.defaults` moves it off the UART
pins.

## Build and flash

The dev image ships ESP-IDF (see `.devcontainer/Dockerfile`); `idf.py` is on
the PATH and needs no `source export.sh`. From the repository root:

```sh
idf.py -C esp32-bridge build                       # -> build/esp32_bridge.bin
idf.py -C esp32-bridge -p /dev/ttyACM0 flash monitor
idf.py -C esp32-bridge fullclean                   # wipe build/
```

The build identity the `Announce` carries (build epoch, short commit hash)
is read by CMake at configure time, so a plain rebuild keeps the previous
pair; `idf.py -C esp32-bridge reconfigure` refreshes it.

`fullclean` keeps `sdkconfig`, so a change to `sdkconfig.defaults` only lands
after `rm esp32-bridge/sdkconfig` (or `idf.py -C esp32-bridge set-target ...`
for the target itself).

`sdkconfig.defaults` pins the `esp32c3` target, the console and the tick rate,
and is the only tracked configuration; `sdkconfig` and `build/` are
generated. The port is whatever the board enumerates as once attached to WSL
with `usbipd` (`/dev/ttyACM*` for the native USB serial of the C3,
`/dev/ttyUSB*` behind a USB-serial chip).

## Using it from the hub

Nothing to do: the board shows up in the hub's discovery like `drone_sim`
does, and the Connect button of its row is the same. The relay sits next to
it as a `relay` node at the same address: nothing to click, a place to read
its logs and turn `relay/stats` on.
