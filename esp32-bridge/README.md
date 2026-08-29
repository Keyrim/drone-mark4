# ESP32 bridge

Firmware for the ESP32-C3 SuperMini (IPEX variant) sitting between the FC and
the ground tools, in place of the USB serial dongle. It is a cable, not a peer:
it carries bytes and never looks at them, so nothing here has to follow the
wire of `protocol/mark4.proto` or its serial framing.

The board joins the network its build named, and raises its own access point,
`mark4-bridge` (WPA2, see `bridge_main.c`), when there is no such network or it
does not answer within ten seconds. Either way it binds UDP port 47830: on the
address the network handed out, or on 192.168.4.1, the ESP-IDF default for an
access point. Which one it is comes out on the console at boot. It stays in the
mode it picked until the next reset; a station that loses its network keeps
asking to join it back.

The network to join is named in `esp32-bridge/local.cmake`, which is untracked
because credentials belong to whoever builds:

```cmake
set(BRIDGE_STA_SSID "my-network")
set(BRIDGE_STA_PASSWORD "my-password")
```

Without that file the bridge is an access point and nothing else, and boots
straight into it. It sends the downlink to whoever last sent it a datagram, so the
ground tool speaks first; the hub does that on its own, every second, from
`SerialTransport`. One ground tool at a time, then: two of them keepaliving
the same bridge steal the stream from each other every second, and each sees
what looks exactly like a lossy radio link. Bytes read on UART1 are gathered until 1024 of them are in or
10 ms have passed, whichever comes first, then leave as one datagram: a client
that lets its radio sleep only receives what the access point could hold for
it, which is a number of datagrams, not a number of bytes. Datagrams
coming the other way are written on the UART as they arrive, keepalives
included: the bridge cannot tell one from a command, and the frame parser on
the board skips whatever is not a frame.

Every second the bridge says it is there, in one broadcast datagram on
udp/47831: the fixed word `mark4-bridge` then a name taken from its MAC
address, sent from the port the board stream travels on. A ground tool listens
there and finds the bridges of the network without being told where they are;
the hub lists them in the "Real board (WiFi)" tab of its control page, and
opening one needs a click and nothing typed. Nothing else ever travels on that
port, and the announce says nothing about the board: it is about the bridge.

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

`sdkconfig.defaults` pins the `esp32c3` target, the console and the tick rate, and is the only
tracked configuration; `sdkconfig` and `build/` are generated. The port is
whatever the board enumerates as once attached to WSL with `usbipd`
(`/dev/ttyACM*` for the native USB serial of the C3, `/dev/ttyUSB*` behind a
USB-serial chip).

## Using it from the hub

Join the `mark4-bridge` network, or put the bridge on the same LAN as the
hub: either way the announce above is what the hub follows, so connecting is
one click on the bridge row. The hub reads and writes the framed stream
straight over UDP and reopens it on its own when the network comes back.
