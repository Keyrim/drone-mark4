# AIO flight controller board - solution requirements

Requirements for the all-in-one (AIO) flight controller board that replaces
the mark1 FC + GY-86 stack: one 30.5 x 30.5 board carrying MCU, IMU,
barometer, blackbox flash and power regulation, stacked on the existing
commercial 4-in-1 ESC. Component selection rationale (prices, stock, PCBA
type, rejected parts) lives in `sensor-board-components.md`; this document
freezes the chosen solution as requirements and lists the open points the
detailed board spec must settle.

Requirement words MUST / SHOULD / MAY follow RFC 2119. IDs are stable;
append, never renumber.

## 1. Scope

- One board: MCU + IMU + baro + NOR flash + 5V/3V3 regulation + connectors.
- The 4-in-1 ESC stays commercial (existing stack, already flown).
- Out of scope, by design:
  - Compass: a board sitting on the ESC battery currents with a buck
    inductor a few mm away is a structurally bad magnetometer host (the
    reason commercial FCs dropped on-board mags). A future GPS module
    carries the mag; the GPS connector exposes I2C for it.
  - Radio link: a future ESP32 bridge module plugs on the telemetry UART
    (see the module shape argued in `sensor-board-components.md`).
  - GPS itself (connector only).

## 2. MCU

- MCU-1 The MCU MUST be the STM32F722RET6 (LCSC C118207, LQFP64,
  Cortex-M7 216 MHz, 512 KB flash, 256 KB RAM). Rationale: cheaper than
  the F405 at current LCSC pricing, same package, current-generation
  peripherals (I2C v2 removes the F4 BUSY erratum class, SPI with FIFO),
  and the de facto FPV industry successor of the F405.
- MCU-2 The SWD header MUST be 6 pins with the mark1 `Prog` pinout
  (3V3, SWCLK, GND, SWDIO, NRST, SWO) so the existing J-Link OB cable
  plugs 1:1.
- MCU-3 BOOT0 MUST be pulled low through a jumper or solder bridge that
  can force it high (system bootloader = free recovery path; mark1 had
  BOOT0 hard-grounded and SWD as the only way in). The pin has no
  internal pull: the pull-down is a real 10k. Firmware convention: the
  BOOT_ADD1 option byte (which points BOOT0=1 at the system bootloader)
  is NEVER reprogrammed, or the recovery path dies.
- MCU-4 HSE MUST be a crystal; 8 MHz is chosen (PLL: /4, x216, /2 gives
  216 MHz sysclk and /9 gives the exact 48 MHz USB clock, so 8 MHz does
  not preclude USB).
- MCU-5 Firmware discipline is unchanged: registers by hand from RM0431,
  no HAL, float-only with -Wdouble-promotion as an error (single
  precision FPU).
- MCU-6 DMA buffers MUST live in SRAM1 with explicit D-cache maintenance
  (or an MPU non-cacheable region); DTCM is the fast-scratch option.
  This replaces the mark1 "no DMA in CCM" rule.

## 3. Sensors

- SNS-1 The IMU MUST be the LSM6DSRTR (LCSC C784817, +/-4000 dps,
  +/-16 g) on a dedicated SPI bus with its own chip select. The single
  shared CS of the mark1 sensor connector is exactly the limitation this
  board removes. Wiring per datasheet Mode 1: SDx and SCx to GND;
  OCS_Aux and SDO_Aux LEFT UNCONNECTED (soldered, no net - they have
  internal pull-ups, grounding them burns current and is out of spec).
  SPI clock 10 MHz max (F722 SPI1 prescaler /16 = 6.75 MHz). Firmware
  note: first writes are I2C_disable=1 (CTRL4_C) and I3C_disable=1 -
  with CS high the I2C/I3C slave is otherwise live on the SPI lines.
- SNS-2 IMU INT1 MUST be routed to an EXTI-capable MCU pin. Using it is
  optional (the loop may stay timer-paced); not routing it would forbid
  data-ready pacing forever. Constraint: INT1 is an interface strap
  sampled at IMU power-up (high = I3C-only mode) - NO pull-up on this
  net ever, MCU pin stays input/low until the IMU is up; an optional
  10k pull-down is the safe default.
- SNS-3 The barometer MUST be the SPA06-003 (LCSC C30589048) on I2C
  (addr 0x76: SDO pulled to GND through 10k rather than shorted, which
  keeps the SDO-as-interrupt option open; CSB to VDDIO). Rate ceiling
  from the datasheet (Rate x Time < 1 s): 25 Hz allows 16x pressure
  oversampling, 50 Hz caps it at 8x.
- SNS-4 Baro placement rules from `sensor-board-components.md` apply:
  pressure port up, no silkscreen or coating on the port, room for an
  open-cell foam patch, away from the regulators (thermal gradient =
  drift) and from prop-wash board edges.
- SNS-5 IMU placement: board center, stiff area, axes aligned with board
  edges and marked on silkscreen, continuous ground plane underneath.
- SNS-6 No magnetometer footprint on this board (see Scope).

## 4. Blackbox storage

- STO-1 The board MUST carry a W25Q128JVSIQ NOR flash (LCSC C97521,
  16 MB, SOIC-8) on its own SPI bus, never shared with the IMU (logging
  writes must not contend with sensor reads). Strapping: /CS gets a 10k
  pull-up to 3V3 (the die has no internal one and the datasheet
  requires /CS to track VCC at power-up, when the MCU pin is still
  high-Z); /WP and /HOLD get 10k pull-ups, NEVER hard ties - the IQ
  order code ships with QE=1, which repurposes them as IO2/IO3, and
  the datasheet explicitly forbids direct supply/ground ties in that
  state.
- STO-2 Sizing: one blackbox record is 65 bytes at 500 Hz = 32.5 kB/s,
  so 16 MB is about 8 minutes of armed logging. Logging only while armed
  is the intended policy. The W25Q256JVSIQ (32 MB, same footprint) MAY
  be substituted at order time.
- STO-3 Plain SPI pins are sufficient; QUADSPI is dropped. Writes are
  bound by flash program time, not bus speed (STO-5), and dumps go over
  USB (CON-5) where full-speed USB (~1 MB/s) is the bottleneck anyway
  (16 MB in ~20-30 s).
- STO-4 Log retrieval goes over USB CDC (CON-5), fallback telemetry
  UART or SWD; no SD card, no USB mass storage requirement. Rationale:
  a throw drone takes 20 g+ impacts; a solder-down SOIC-8 survives
  them, an SD socket is a mechanical liability, and FAT is a firmware
  project of its own.
- STO-5 Throughput analysis (why the bus is never the constraint):
  required sustained rate is 32.5 kB/s (STO-2). The W25Q128JV programs
  a 256-byte page in 0.7 ms typical / 3 ms max, i.e. 366 kB/s typical
  and 85 kB/s worst-case sustained - above the requirement even at
  datasheet max. The SPI clock is irrelevant to writes (50 MHz ceiling
  for plain 03h reads, 27 MHz chosen on SPI2); what matters is erase
  latency: a 4 KB sector erase stalls the die 45 ms typical, up to
  400 ms max, which is 13 kB of backlog at the required rate. The
  firmware MUST therefore buffer records in a RAM ring (16 KB or more,
  cheap out of 256 KB) and erase ahead of the write head.

## 5. Power

- PWR-1 Main input is the ESC stack rail: regulated 12 V (confirmed).
  The chosen AP63205WU-7 (5 V fixed, 2 A, 3.8-32 V input) keeps huge
  margin regardless.
- PWR-2 3V3 MUST come from a LDO fed by the 5 V rail (clean supply for
  the sensors), 500 mA class.
- PWR-3 The bench path MUST work without the ESC rail: USB-C VBUS
  (CON-5) is the bench 5 V, ORed with the buck output so plugging both
  never back-feeds either side. Topology constraint: the blocking
  element MUST sit in the buck's own output leg too, not only on the
  USB side - the AP63205 is a synchronous buck whose high-side body
  diode conducts from output to input, so 5 V forced on its output
  back-drives the 12 V net to ~4.3 V, above the chip's own UVLO
  (3.3-3.7 V), and it would try to self-start. Two Schottkys (or an
  ideal-diode ORing part) solve it; budget the drop: the shared rail
  sits at ~4.6-4.7 V, fine for the 3V3 LDO and the CON-1 5 V pin.
- PWR-4 A voltage sense divider on the 12 V input into an ADC pin MUST
  be present (two resistors; monitors the rail the board actually lives
  on). The divider MUST clamp below VDDA at the rail maximum, and the
  ADC input gets a Schottky to ground: DS11853 Table 60 allows ZERO
  negative current injection on PC0..PC3. Note: the rail being
  regulated, battery level is NOT visible there; battery
  voltage/current telemetry comes from the ESC telemetry UART (ESC-2)
  if the ESC provides it. An ESC current-sense input (CURR pin) SHOULD
  get at least a pad.
- PWR-5 Budget sanity: MCU ~100 mA + sensors ~2 mA + flash write ~25 mA
  + LEDs ~10 mA is under 200 mA on 3V3; the 2 A buck also leaves room
  for a future ESP32 module (~500 mA bursts) on the 5 V rail. Thermal
  honesty: the AP63205 derating curve holds 2 A only to ~60 C ambient
  (TSOT26, 89 C/W) - fine at our load, but "2 A" is not usable headroom
  inside a hot ESC stack.

## 6. Motor and ESC interface

- ESC-1 The four motor outputs MUST be the four channels of a single
  DMA-capable advanced timer, so DShot is a firmware upgrade, not a
  respin. DMA buffers per MCU-6. The instance is TIM8 (TIM1 is
  impossible on the 64-pin package with USB present - see the resource
  budget in `aio-board-design.md`).
- ESC-2 An ESC telemetry UART RX SHOULD be wired to the ESC connector.
- ESC-3 The ESC connector reuses the mark1 board pinout (confirmed);
  copy it from the mark1 schematic at capture time.

## 7. External connectors

All external connectors are JST-SH 1 mm (mark1 habit, cables exist).

- CON-1 Telemetry UART (USART1): 4 pins GND / 5V / RX / TX. Serves the
  FTDI bench link today and the ESP32 bridge module later.
- CON-2 GPS connector: UART + I2C + 3V3/GND (future GPS + external mag).
- CON-3 One spare UART connector SHOULD be present (RC receiver, e.g.
  ELRS, when a real radio arrives).
- CON-4 SWD per MCU-2.
- CON-5 A USB-C receptacle MUST be present for bench data and bench
  power: USB 2.0-only 16-pin connector, 5.1k pulldowns on both CC pins
  (UFP), ESD protection array on D+/D-, wired to the F722 OTG_FS pins
  (PA11/PA12). Roles: CDC serial (telemetry and blackbox dump, STO-4),
  DFU via the system bootloader (with MCU-3 BOOT0 jumper), VBUS as the
  bench supply (PWR-3). MCU-4 already guarantees the 48 MHz USB clock.

## 8. Indicators

- IND-1 Two status LEDs MUST be present (the firmware LED policy
  already encodes phases on one LED and gains a second one back).
- IND-2 A buzzer driver (GPIO + transistor + footprint) MUST be present
  (find-me and arm beeps).

## 9. Mechanical

- MEC-1 30.5 x 30.5 mm M3 stack pattern, mounted on the ESC silicone
  grommets (they are the IMU vibration isolation).
- MEC-2 SWD and telemetry connectors MUST stay reachable with the board
  stacked (board edge, not facing the ESC wiring).
- MEC-3 Baro and IMU placement per SNS-4/SNS-5.

## 10. Sourcing and assembly

- SRC-1 JLCPCB Standard PCBA (the LGA sensors force it and X-ray), 4
  layers, one side assembled. Basic parts wherever possible; every
  Extended part MUST be LCSC in-stock at order time. The selected
  references live in `aio-board-components.md`.
- SRC-2 Budget envelope: about 120 EUR for 3 assembled boards, about
  150 EUR for 5, shipping Global Standard (never the preselected express
  line). Cost model and quote history in `sensor-board-components.md`.
- SRC-3 Footprints are the requirement, not populated parts: anything
  not flight-critical (buzzer, spare connectors, current-sense input)
  MAY be marked DNP at assembly time to trim the quote.

## 11. Open points for the detailed board spec

Resolved 2026-08-12: O-1 rail is regulated 12 V (PWR-1), O-2 reuse the
mark1 ESC pinout (ESC-3), O-3 USB-C is in (CON-5), O-4 buzzer is in
(IND-2), O-5 plain SPI, QUADSPI dropped (STO-3/STO-5).

Resolved 2026-08-12 (bis): O-6 the board is named "mark4-fc", first
revision "rev A".

- O-7 Full pin map of the F722 (every requirement above lands on a
  named pin, alternate functions checked for conflicts), on top of the
  resource budget in `aio-board-design.md`.
