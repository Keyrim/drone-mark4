# ESP32 bridge

Firmware for the ESP32-C3 SuperMini (IPEX variant) sitting between the FC and
the ground tools. The transparent UART (921600 baud to 2 Mbaud) <-> UDP WiFi
bridge is not written yet: this is a heartbeat skeleton that logs one line per
second, no WiFi, no UART, no GPIO. When the bridge exists it will speak only
`protocol/`.

## Build and flash

The dev image ships ESP-IDF (see `.devcontainer/Dockerfile`); `idf.py` is on
the PATH and needs no `source export.sh`. From the repository root:

```sh
idf.py -C esp32-bridge build                       # -> build/esp32_bridge.bin
idf.py -C esp32-bridge -p /dev/ttyACM0 flash monitor
idf.py -C esp32-bridge fullclean                   # wipe build/ and sdkconfig
```

`sdkconfig.defaults` pins the `esp32c3` target and is the only tracked
configuration; `sdkconfig` and `build/` are generated. The port is whatever the
board enumerates as once attached to WSL with `usbipd` (`/dev/ttyACM*` for the
native USB serial of the C3, `/dev/ttyUSB*` behind a USB-serial chip).

The console target is left at the ESP-IDF default. The SuperMini exposes the
USB Serial/JTAG peripheral on its USB-C port, so seeing `monitor` output may
need `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: to be confirmed on the real board.
