# ESP32 relay

Firmware for the ESP32-C3 SuperMini (IPEX variant) riding the drone, wired
to the flight controller's UART. It is a transport relay
(`software/components/transport/`): one transport node with two links, the
board's UART and the WiFi LAN, forwarding frames between them. It has no
identity anybody learns: it never beacons, so what the hub sees is the
board's own `Announce`, at the relay's IP address and data port. From the
LAN's point of view the board is one more node on udp/47820, exactly like
`drone_sim`; the hub has no path, port or click specific to it.

## What crosses, and what does not

The relay forwards with the transport's generic rules (split horizon, one
hop less per relay, duplicate drop by `(src, seq)`), plus one outbound
filter on the UART link, `uartFilter()` in `main/relay.cpp`:

- towards the LAN: everything the board emits (telemetry, answers, log
  lines, its Announce), as broadcasts;
- towards the UART: every unicast the transport routes there (the board is
  the only node on that link, so a unicast routed there is for it: RC,
  tuning, updater messages), and the broadcasts whose envelope body is an
  `Announce` (the board learns the LAN nodes from those). Every other LAN
  broadcast, `drone_sim`'s telemetry and run stats first, stays on the LAN:
  a 921600 baud line does not carry the whole LAN.

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
  the filter, no `setBeacon()`.

The main task polls the transport once per FreeRTOS tick (1 ms,
`CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults`): one frame is one
datagram, no aggregation. Every poll drains both links whole; a 160-byte
telemetry frame takes 1.7 ms on the line, so the added latency is under one
frame time either way. The UART driver keeps 4096 bytes on the way in
(about 44 ms of a full line, what a WiFi stall may cost before bytes are
lost and the framing resynchronizes) and 1024 on the way out.

The shared sources are compiled by the ESP-IDF build straight from
`software/components/`: `transport.cpp`, `uart_link.cpp`,
`posix/udp_link.cpp` in the `main` component, the nanopb runtime plus the
codec generated from `software/components/protocol/mark4.proto` in
`components/mark4_proto/` (same generator, same pinned nanopb commit, same
`PB_NO_MALLOC PB_BUFFER_ONLY` as the desktop build; the generator needs a
python with `protobuf` and `grpcio-tools`, the component uses the first of
the IDF interpreter and `/usr/bin/python3` that has them). The `main`
component carries the project's warning set (`-Wall -Wextra -Wconversion
-Wdouble-promotion -Werror`) and `-fno-exceptions -fno-rtti`.

## Console

USB Serial/JTAG, 115200. What a healthy relay prints:

```
I bridge: joined <ssid> as 192.168.1.31            (or: access point mark4-bridge)
I relay: relay up: node ff42bd55, uart 921600 baud, lan data port 60278, discovery port 47820
I relay: node 6c41b2f0 up on the uart              (the board)
I relay: node 1cd5e199 up on the lan at 192.168.1.13:41283   (the hub)
I relay: nodes 2, relayed 1063, filtered 250, dropped 0, uart tx full 0   (every 5 s)
I relay: node 1cd5e199 gone                         (3 s of silence)
```

`relayed` counts frames forwarded (one per link for a broadcast), `filtered`
the LAN broadcasts the filter kept off the UART, `dropped` the transport's
drops (unknown destination, hops exhausted), `uart tx full` the frames the
UART ring could not take whole.

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
does, and the Connect button of its row is the same. The relay itself never
appears anywhere, which is the point.
