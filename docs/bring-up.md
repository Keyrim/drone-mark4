# Board bring-up - mark1 hardware

Reference for running the firmware on real hardware: the custom mark1
flight controller, a reflashed ST Discovery as debug probe, and a
commercial IMU breakout. Everything runs from the devcontainer.

## Hardware

### Flight controller: mark1

Custom board (schematics and docs: https://github.com/Keyrim/drone-stm32f4-mark1,
see `Flight_Controller/Board/Docs/Flight_Controller_Schematic.md`).

- **STM32F405RGT6**, 8 MHz HSE crystal - the exact chip the `stm32`
  preset, `startup.c` and `stm32f405_image.ld.in` already target.
- **BOOT0 tied to GND**: no USB, no DFU. Flashing goes through SWD only.
- LDO 5V -> 3V3 (MCP1826). Status LEDs: LED1 = PC13, LED2 = PC14.
  **LED2 is dead on this unit** (PC14 measures 3.3 V on IDR when driven,
  the LED never lights: dead, reversed or never fitted): all the LED
  signalling goes through LED1 (see `software/drone_firmware/status_leds.hpp`).
- `Prog` header, 6 pins: `3V3, SWCLK, GND, SWDIO, NRST, SWO` - the same
  pinout as the CN2 connector of ST Discovery boards, so the probe cable
  is 1:1.
- UART1 on PB6/PB7 (connector `uart1`), UART3/UART4 free, UART6 on the
  GPS connector. I2C1 on PB8/PB9 with 4.7k pull-ups on the board.

### Power

The mark1 Power board was never produced. The board is powered through
an **FTDI USB-UART dongle**: its 5V goes into the `+5` pin of a UART
connector, which feeds the 5V rail and the LDO. The same dongle carries
the UART telemetry.

### Sensors

The mark1 Sensors board is not used. A commercial **GY-86 breakout**
(identified on the bus by the bring-up firmware) is wired to the
`sensor_board` connector and speaks on **I2C1**:

- **MPU6050** at 0x68 (confirmed by WHO_AM_I; no ID register on the
  MS5611, its PROM answered instead).
- **MS5611** barometer at 0x77. **The cell is faulty (or counterfeit)**:
  the I2C protocol is fully functional (PROM CRC valid, conversions
  answer, a second ADC read returns 0 exactly per the datasheet), but D1
  sits at 99 % of the ADC full scale and the solved pressure is negative.
  The driver gates implausible solutions and the frame carries 0 Pa; the
  breakout needs replacing before altitude work.
- **HMC5883L** compass at 0x1E, wired behind the MPU6050 auxiliary bus:
  it only appears on the main bus once the MPU I2C bypass
  (INT_PIN_CFG.I2C_BYPASS_EN) is open.

Constraints this hardware puts on the firmware:

- The MPU interrupt line is **not wired**: the main loop is paced by a
  timer interrupt (WFI on the timer), not by the gyro data-ready. The
  SensorFrame timestamp comes from that timer.
- The MPU6050 gyro saturates at +/-2000 deg/s (~35 rad/s): the flight
  core's gyro saturation cutoff must sit below that, and saturation must
  be handled. Measured hand throws tumble at <= 10 rad/s, well within
  range.
- No SD or flash chip: the blackbox streams over UART (or RTT) to the
  PC for now.

## Debug probe

An old ST Discovery reflashed to a **J-Link OB** (SEGGER STLinkReflash).
Remove the two ST-LINK jumpers (CN3) so the probe drives the external
target instead of the on-board MCU, then wire CN2 to the `Prog` header
pin for pin.

## Container access to the probe

The probe and the FTDI dongle are attached to WSL, then reach the
container through the `/dev` bind mount (see `.devcontainer/`):

```powershell
# Windows side, once per plug-in
usbipd list
usbipd attach --wsl --busid <BUSID>
```

The J-Link tools live in the image (`/opt/SEGGER/JLink`). The usbipd
device node comes up root-only, which blocks the J-Link tools as the
container user. Persistent fix, once in the WSL distro (udev runs there,
not in the container; the rules file is extracted in the image):

```sh
# WSL side (grab the file from the container, then reload udev)
docker cp <container>:/etc/udev/rules.d/99-jlink.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
# then detach / re-attach the probe with usbipd
```

Quick one-shot alternative, from inside the container (the /dev bind
mount makes it act on the WSL node, lost at the next re-plug):

```sh
sudo chmod 666 /dev/bus/usb/<bus>/<dev>
```

## Flash and logs

```sh
# Build
cmake --preset stm32 && cmake --build --preset stm32

# Flash (J-Link commander)
JLinkExe -device STM32F405RG -if SWD -speed 4000 -autoconnect 1
# then: loadfile software/build/stm32/drone_firmware/drone_firmware.elf, r, g

# Or: gdb server + gdb
JLinkGDBServer -device STM32F405RG -if SWD -speed 4000 &
gdb-multiarch software/build/stm32/drone_firmware/drone_firmware.elf \
    -ex "target extended-remote localhost:2331"

# RTT console (printf-style logs, no UART needed)
JLinkRTTClient
```

## Bring-up sequence

Incremental, one observable win per step:

1. **Clock + heartbeat + RTT + I2C scan**: 168 MHz from the 8 MHz HSE,
   LED1 blinking, a hand-written RTT up-buffer for logs, and an I2C1
   scan printing the addresses found on the breakout.
2. **IMU driver**: MPU6050 over I2C, timer-paced sampling into
   SensorFrames; barometer driver for whatever the scan identified.
3. **Telemetry**: SensorFrames streamed over UART1 through the FTDI to
   the PC; blackbox over the same link. Done: 50 Hz
   TelemetryPacket stream plus full-rate blackbox records at 921600
   baud (about 40 % of the line), each wrapped in the serial framing of
   `protocol/serial_framing.hpp` (a UART has no datagram boundaries),
   interrupt-driven behind a ring buffer, demuxed by payload size on
   the PC. The `hub` (UART opened from the Board panel of the control
   page) is the
   single serial consumer of a session: it re-broadcasts telemetry over
   UDP, serves the web pages that plot it, and captures `.m4bb` files
   that `drone_replay` plays back. The uplink carries the pilot state
   (RcCommandPacket: kill, arm, throttle): an `rc` message aimed at
   `firmware` on the hub websocket endpoint is framed onto the UART
   verbatim, and 500 ms of silence trips the fail-safe (kill engaged,
   disarmed), so closing the sender is itself a safe action. A simulated
   flight is flown the same way: an `rc` message aimed at `drone_sim`
   goes to udp/47805, where `drone_sim` binds its own command receiver,
   so the RC path and its fail-safe are exercised in every simulated
   flight, not only on the bench. The simulator holds no pilot state of
   its own - it is the plant, not the cockpit - and its keyboard only
   drives the world (H hold, SPACE throw, R reset). A `reboot` message
   reboots the board (NVIC
   system reset), forwarded by the hub like any other uplink packet;
   no simulator key is wired to it.
4. **Detection on real hands**: board armed and shaken in hand (no
   false spin-up expected), then thrown and caught - throw detection
   and apex prediction on real sensor data, compared against the
   simulator campaign metrics.

No vendor HAL: registers are defined by hand (RM0090), only the ones
actually used. The old mark1 firmware (`Flight_Controller/Software/`)
is a register-map reference, not a dependency.
