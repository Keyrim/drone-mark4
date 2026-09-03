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
- MCU-2 The SWD header is a through-hole 2.54 mm 3-position footprint,
  pinout SWDIO / SWCLK / GND, NOT assembled: hand-populated from bench
  stock. Three pins are what the whole mark1 bring-up actually used -
  the J-Link OB VTref is hardcoded, the homemade RTT runs over SWD
  memory access (no SWO needed), and BOOT0 (MCU-3) is the recovery path
  NRST would have covered. Dropping the 3V3, NRST and SWO positions
  (2026-08-18) buys 7.6 mm of board edge, which is what lets SWD and
  USB-C share one edge (MEC-2) on a 36 mm board; the price is the mark1
  `Prog` 1:1 cable fit and connect-under-reset, and PB3/SWO becomes an
  unused pin.
- MCU-3 BOOT0 MUST be pulled low through a jumper or solder bridge that
  can force it high (system bootloader = free recovery path; mark1 had
  BOOT0 hard-grounded and SWD as the only way in). The pin has no
  internal pull: the pull-down is a real 10k. Firmware convention: the
  BOOT_ADD1 option byte (which points BOOT0=1 at the system bootloader)
  is NEVER reprogrammed, or the recovery path dies.
- MCU-4 HSE MUST be a crystal; 8 MHz is chosen (PLL: /4, x216, /2 gives
  216 MHz sysclk and /9 gives the exact 48 MHz USB clock, so 8 MHz does
  not preclude USB).
- MCU-5 Firmware discipline is unchanged: registers from the vendored
  CMSIS device header, no HAL, float-only with -Wdouble-promotion as an
  error (single precision FPU).
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
  is the intended policy. A 32 MB part MAY be substituted at order time,
  but NOT the W25Q256JVSIQ: Winbond never shipped the 256 Mbit die in
  SOIC-8 208 mil (only WSON-8, SOIC-16 300 mil and TFBGA-24), so that
  order code does not exist. The drop-in on this land pattern is the
  Puya PY25Q256HB-SUH-IT (LCSC C50201614, SOP-8 208 mil, W25Q-compatible
  pinout with HOLD on pin 7). Any 256 Mbit part crosses the 16 MB
  24-bit addressing limit, so firmware MUST issue EN4B once at init and
  use 4-byte address commands - that, not the price, is the cost of the
  upgrade. 512 Mbit is not an option here: no vendor has one in any
  8-pin SOP at JLCPCB.
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

- PWR-1 Main input is the ESC BEC rail: regulated 10 V on the T-Motor
  Velox 45A V2 (updated 2026-08-16; the old "regulated 12 V" came from
  the mark1 stack). The board MUST NOT feed from raw VBAT: motor
  braking spikes on a 6S pack can exceed the AP63205 32 V input
  rating, and the ESC BEC absorbs them for free. The chosen
  AP63205WU-7 (5 V fixed, 2 A, 3.8-32 V input) keeps huge margin at
  10 V.
- PWR-2 3V3 MUST come from a LDO fed by the 5 V rail (clean supply for
  the sensors), 500 mA class.
- PWR-3 The bench path MUST work without the ESC rail: USB-C VBUS
  (CON-5) is the bench 5 V, ORed with the buck output so plugging both
  never back-feeds either side. Topology constraint: the blocking
  element MUST sit in the buck's own output leg too, not only on the
  USB side - the AP63205 is a synchronous buck whose high-side body
  diode conducts from output to input, so 5 V forced on its output
  back-drives the input net to ~4.3 V, above the chip's own UVLO
  (3.3-3.7 V), and it would try to self-start.
  The OR-ing element is an active power mux, not two Schottkys
  (decided 2026-08-17, supersedes the "two Schottkys" of 2026-08-12):
  a TPS2116, 1.6-5.5 V, 2.5 A, 40 mOhm typical and 60 mOhm over
  temperature, which blocks reverse current in both legs. Two
  consequences that matter: the shared rail sits at ~4.98 V instead of
  the ~4.6-4.7 V a Schottky pair left (which the GPS of CON-2 needs,
  4 to 6 V on its own LDO), and the diode heating that used to cap the
  rail current is gone.
  Configuration: VIN1 = buck output and MODE tied to VIN1, which
  selects priority mode, so the BEC wins whenever it is present and
  plugging or unplugging USB never disturbs a powered board; VIN2 =
  VBUS; a resistor divider from VIN1 into PR1 sets the switchover
  threshold at 4.03 V (VREF is 0.92 to 1.08 V, so 3.7 to 4.35 V worst
  case, far below the buck's 5 V +/-2 %). When VIN1 is dead (bench,
  USB only) MODE and PR1 are both low and the truth table passes the
  higher input, so the board runs on USB with nothing to command.
  The status output (ST, open drain) MUST be pulled up and wired to a
  GPIO: the firmware then knows whether it is on the BEC or on bench
  power. Two costs of the active part, accepted: switchover is
  BREAK-before-make (tSW = 8 us at 5 V), so the shared rail MUST carry
  enough bulk to ride the gap - 32 uF holds the droop near 145 mV at
  400 mA where 10 uF would drop 457 mV; and VIN absolute maximum is
  6 V (5.5 V recommended), so unlike the old Schottky stage this node
  is no longer indifferent to a USB overvoltage.
  No extra TVS is fitted on VBUS for that, and the reasoning is worth
  keeping (examined 2026-08-17). The USBLC6-2SC6 already on the board
  (CON-5) protects VBUS as well as the data lines: its rail element
  breaks down at 6.1 V and clamps to 11-13 V at 8 A, 8/20 us. Adding a
  discrete 5 V TVS would move that clamp to about 9.8 V and no further,
  because no passive part can hold 5.25 V (the legal VBUS maximum)
  and cap at 6 V - a TVS needs a factor of roughly two between its
  working and clamping voltages, and the window here is 14 %. So a TVS
  cannot make the absolute maximum be respected; it only turns a 25 V
  spike into a 10 V one, which the existing part already does. The
  case it would not cover either is a sustained overvoltage (a faulty
  source pushing 9 or 12 V), where a TVS just heats up and dies: that
  needs an active OVP switch, a separate decision with its own part and
  area. It is also a remote case, since the 5.1k CC pulldowns make this
  a UFP with no PD negotiation, so a compliant source never leaves 5 V.
- PWR-4 A voltage sense divider on the raw VBAT pad into an ADC pin
  MUST be present (two resistors; the Velox harness exposes VBAT, so
  battery voltage is measured directly - updated 2026-08-16, the
  divider used to watch the regulated input rail where battery level
  was invisible). The divider MUST clamp below VDDA at 25.2 V (6S
  max), and the ADC input gets a Schottky to ground: DS11853 Table 60
  allows ZERO negative current injection on PC0..PC3. Battery current
  telemetry comes from the ESC telemetry UART (ESC-2) if the ESC
  provides it. An ESC current-sense input (CURR pad) SHOULD get at
  least a pad.
- PWR-5 Budget sanity: MCU ~100 mA + sensors ~2 mA + flash write ~25 mA
  + LEDs ~10 mA is under 200 mA on 3V3; the 2 A buck also leaves room
  for a future ESP32 module (~500 mA bursts) on the 5 V rail. Thermal
  honesty: the AP63205 derating curve holds 2 A only to ~60 C ambient
  (TSOT26, 89 C/W) - fine at our load, but "2 A" is not usable headroom
  inside a hot ESC stack.
  Updated 2026-08-17 with the two external loads now planned. GPS
  (CON-2, Matek M9N-5883): 50 mA typical at 4-6 V, negligible. Whole
  5 V rail with the ESP32: ~400 mA continuous, ~770 mA peak, which is
  220 mA / 2.2 W continuous and 430 mA / 4.3 W peak seen from the 10 V
  BEC - about a fifth of the Velox BEC's 20 W, and the BEC is never the
  binding constraint. The LED strip (IND-3) is deliberately NOT part of
  that budget: it draws from the buck output ahead of the mux, so its
  current never crosses the mux and never appears on the shared rail.
  Derate the BEC anyway: on a 4-in-1 it shares the heatsink with the
  MOSFETs, so its 2 A rating on the bench is not 2 A in aggressive
  flight. The ESP32 and the strip are both bursty and their peaks can
  coincide; bulk decoupling for a module at the end of a 20 cm harness
  belongs AT the module, not on this board, where the wire inductance
  would defeat it.

## 6. Motor and ESC interface

- ESC-1 The four motor outputs MUST be the four channels of a single
  DMA-capable advanced timer, so DShot is a firmware upgrade, not a
  respin. DMA buffers per MCU-6. The instance is TIM8 (TIM1 is
  impossible on the 64-pin package with USB present - see the resource
  budget in `aio-board-design.md`).
- ESC-2 An ESC telemetry UART RX SHOULD be wired to the ESC connector.
- ESC-3 The ESC interface is solder pads, not a connector (decided
  2026-08-16, supersedes the 8-position JST-SH frozen 2026-08-15: the
  4-in-1 pinout is not standardized across brands - order, pin count
  and BEC presence all vary - so a frozen connector pinout buys
  nothing; the harness is cut and soldered once, wire order verified
  with a continuity check). Nine individual pads (J40-J48, since
  2026-08-17 - it used to be a 1x09 row): GND, VIN (ESC BEC, PWR-1),
  VBAT (sense divider, PWR-4), M1..M4, CURR (analog current sense,
  DNP-able fallback per SRC-3), TLM (ESC telemetry UART RX, ESC-2).
  Going individual removed a real hazard rather than only saving edge:
  there is no longer a pad ORDER that could be confused with the ESC-4
  connector numbering, since each pad states its signal. Placement
  SHOULD follow the measured Velox wire order so that pads and
  connector read alike. Reference harness, T-Motor Velox 45A V2 (JST-SH 1 mm,
  10 wires): GND, GND, VBAT, 10V, M4, M3, M2, M1, CRT, TX.
  The pads are PLATED THROUGH-HOLE (resolved 2026-08-16, after a
  round that had drawn them SMD): the barrel takes the mechanical
  load, and the pad is reachable and silkscreen-labelled from BOTH
  faces, so the wire is soldered from whichever side suits the build.
  Reference: T-Motor F7 (F722), whose whole IO row is plated holes
  labelled on both sides. No strain relief feature is added; the
  harness is captured by the stack and the wires are short.
- ESC-4 The ESC ALSO gets a plug-in connector in parallel with the
  ESC-3 pads (decided 2026-08-16): a 10-position JST-SH side-entry
  receptacle wired to the same nets, at the measured Velox wire
  order, so that harness plugs straight in with no soldering and
  stays removable. The two paths divide as follows: the connector is
  the bench path, the pads are the flight path (the SH has no lock
  and can walk out under vibration) and the brand-agnostic path (any
  other 4-in-1). NOTE that the connector is in Velox wire order while
  the pads are in ESC-3 spec order, so the two are numbered
  differently on purpose; the silkscreen MUST make this unambiguous.
  No other interface gets a connector footprint: CON-1, CON-2 and
  CON-3 are pads only.

## 7. External connectors

External IO is solder pads, not connectors (decided 2026-08-16,
supersedes "all JST-SH 1 mm"): no crimping, no connector body depth
(the JST depth is what forced the 48 x 48 outline), no feeder fees,
and a soldered wire holds vibration better than the lock-less SH.
The pads are plated through-holes, labelled on both faces, per
ESC-3. The only connectors left are USB-C (CON-5), the SWD
through-holes (CON-4) and the ESC receptacle of ESC-4 - and no
connector footprint is provided "just in case" anywhere else: an
unpopulated footprint for hardware that does not exist yet would
freeze a pinout we have no reason to trust.

Each pad is INDIVIDUAL, with no fixed geometry (decided 2026-08-17,
supersedes the 1xN pad groups of 2026-08-16). One footprint,
`SolderPad_1x01_THT`, is instantiated once per pad, and its silkscreen
label is the component's VALUE field, drawn vertically on both faces,
so every pad names its own signal (TX1, M1, VBAT, ...). What this buys
is placement freedom: external IO no longer reserves a fixed length of
board edge - the 1xN groups added up to 66.4 mm of edge in five
indivisible blocks, which on the 40 x 40 outline of the day forced the
distribution over the four sides and would not fit at all on the
36 x 36 the board now is. A pad can slip between mounting holes, and a
second row is usable because the holes are plated and labelled on both
faces (the back row is soldered from below). The pads of one interface
SHOULD still be placed together and in a sensible order - the freedom
is not an invitation to scatter them - but the tool no longer imposes
it, and nothing but the silkscreen guarantees it, so check placement by
eye (MEC-5). What actually gets wired is decided at soldering time.

- CON-1 Telemetry UART (USART1): 4 pads, labelled GND / 5V / RX1 / TX1
  (J10-J13). Serves the FTDI bench link today and the ESP32 bridge
  module later.
- CON-2 GPS pads, 7 of them, labelled GND / 5V / TX4 / RX4 / SCL / SDA
  / 3V3 (J20-J26; GPS + external mag). Updated 2026-08-17: the group used to carry
  3V3 only, frozen before a module was picked. The reference module is
  a Matek M9N-5883 (u-blox NEO-M9N + QMC5883L), which wants 4 to 6 V
  on its own LDO, as does every commercial GPS module, so 5 V MUST be
  present; 3V3 is kept on a seventh pad so a bare 3.3 V receiver stays
  wireable. The 5V and 3V3 pads MUST NOT be placed adjacent: a
  5V-to-3V3 solder bridge would put 5 V on the 3V3 rail and kill MCU,
  IMU, baro and flash at once, while a 3V3-to-SDA bridge only stalls a
  bus. (Before pads went individual this was worded as "opposite ends
  of the row"; the constraint is the same, only the row is gone.)
  One thing to know before wiring an external magnetometer: the board's
  own I2C1 pull-ups (R6/R7, 4.7k to 3V3, on the Sensors sheet) end up
  in parallel with the ones the GPS module carries, and the harness
  adds 20-30 cm of wire. That is the standard arrangement on every
  flight controller and works at 100 kHz; if a module turns out to
  pull hard, raise R6/R7 rather than adding anything.
- CON-3 One spare UART SHOULD be present (RC receiver, e.g. ELRS, when
  a real radio arrives): 4 pads GND / 5V / RX3 / TX3 (J30-J33).
- CON-4 SWD per MCU-2.
- CON-5 A USB-C receptacle MUST be present for bench data and bench
  power: USB 2.0-only 16-pin connector, 5.1k pulldowns on both CC pins
  (UFP), ESD protection array on D+/D-, wired to the F722 OTG_FS pins
  (PA11/PA12). Roles: CDC serial (telemetry and blackbox dump, STO-4),
  DFU via the system bootloader (with MCU-3 BOOT0 jumper), VBUS as the
  bench supply (PWR-3). MCU-4 already guarantees the 48 MHz USB clock.

The LED strip group of IND-3 is external IO as well and follows the
same rules (solder pads, labelled on both faces); it is specified with
the indicators because that is what it drives. It does not contradict
the "no footprint just in case" rule above: a labelled pad row freezes
no vendor pinout, whereas a connector body would.

## 8. Indicators

- IND-1 Two status LEDs MUST be present (the firmware LED policy
  already encodes phases on one LED and gains a second one back).
- IND-2 A buzzer driver (GPIO + transistor + footprint) MUST be present
  (find-me and arm beeps).
- IND-3 Addressable LED strip pads MUST be present (added 2026-08-17):
  3 individual pads labelled GND / LED5V / LED (J50-J52), with a series
  resistor on the data line. The 5 V pad is named LED5V and not 5V on
  purpose: it is a different net from the other 5V pads, see below. Two decisions are load-bearing here.
  First, the 5 V pad is taken at the buck output, AHEAD of the PWR-3
  power mux (net `+5V_BUCK`, not `+5V`): strip current then never
  crosses the mux, and a welcome side effect is that the strip cannot
  light up on USB power alone.
  Second, the data pin MUST be a timer channel with a DMA request of
  its own, because a WS2812 bit stream is timer + DMA, not bit-banged
  GPIO. The instance is TIM1_CH3 on DMA2 S6C6 - see the resource budget
  in `aio-board-design.md` for why TIM3 and TIM4 could not do it.
  No level shifter is provided: a WS2812 asks for 0.7 x VDD, so a 3.3 V
  drive is marginal per its datasheet even though it is what commercial
  flight controllers all do; if a strip refuses to latch, the fix is to
  drop the first LED's supply with a series diode, not to respin.

## 9. Mechanical

- MEC-1 30.5 x 30.5 mm M3 stack pattern, mounted on the ESC silicone
  grommets (they are the IMU vibration isolation). The outline is
  36 x 36 mm with 2 mm corner fillets (2026-08-18): that is the floor
  the pattern allows, since a hole 15.25 mm off centre needs its
  2.65 mm grommet keepout to stay on the board (15.25 + 2.65 = 17.9).
- MEC-2 The SWD header and the USB-C receptacle MUST sit on the same
  board edge, and that edge faces a lateral side of the drone: both
  stay reachable with the board stacked and the frame on, away from
  the ESC wiring (updated 2026-08-16).
- MEC-3 Baro and IMU placement per SNS-4/SNS-5.
- MEC-4 The silkscreen MUST carry an arrow marking the expected board
  orientation in the frame (flight forward direction).
- MEC-5 Silkscreen text only where it stays readable: pad labels
  first, reference designators only where a legible size fits - no
  refdes is better than an unreadable one. Since pads went individual
  (section 7) the label IS the pad's identity, so a pad whose label is
  unreadable or ambiguous is a wiring mistake waiting to happen; the
  courtyard does not cover the label, so DRC will not catch an overlap
  and it MUST be checked by eye.
- MEC-6 The bottom side is available: SRC-1 assembles the top side
  only, so hand-soldered parts, pads and silkscreen may use the
  bottom freely.

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
(IND-2), O-5 plain SPI, QUADSPI dropped (STO-3/STO-5). O-1 and O-2
were superseded 2026-08-16 by the measured T-Motor Velox 45A V2
harness (10 V BEC, 10-wire pinout) and the pads decision - see PWR-1
and ESC-3.

Resolved 2026-08-12 (bis): O-6 the board is named "mark4-fc", first
revision "rev A".

- O-7 Full pin map of the F722 (every requirement above lands on a
  named pin, alternate functions checked for conflicts), on top of the
  resource budget in `aio-board-design.md`.
