# Board bring-up - mark1 hardware

Reference for running the firmware on real hardware: the custom mark1
flight controller, a reflashed ST Discovery as debug probe, and a
commercial IMU breakout. Everything runs from the devcontainer.

## Hardware

### Flight controller: mark1

Custom board (schematics and docs: https://github.com/Keyrim/drone-stm32f4-mark1,
see `Flight_Controller/Board/Docs/Flight_Controller_Schematic.md`).

- **STM32F405RGT6**, 8 MHz HSE crystal - the exact chip the `stm32`
  preset, `startup.c` and `stm32f405.ld` already target.
- **BOOT0 tied to GND**: no USB, no DFU. Flashing goes through SWD only.
- LDO 5V -> 3V3 (MCP1826). Status LEDs: LED1 = PC13, LED2 = PC14.
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
- **MS5611** barometer at 0x77.
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
# then: loadfile build/stm32/apps/firmware/firmware.elf, r, g

# Or: gdb server + gdb
JLinkGDBServer -device STM32F405RG -if SWD -speed 4000 &
gdb-multiarch build/stm32/apps/firmware/firmware.elf \
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
   the ground station; blackbox over the same link.
4. **Detection on real hands**: board armed and shaken in hand (no
   false spin-up expected), then thrown and caught - throw detection
   and apex prediction on real sensor data, compared against the
   simulator campaign metrics.

No vendor HAL: registers are defined by hand (RM0090), only the ones
actually used. The old mark1 firmware (`Flight_Controller/Software/`)
is a register-map reference, not a dependency.
