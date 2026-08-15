# mark4-fc rev A - status and open topics

Living tracking document for the AIO flight controller board. Requirements
live in `aio-board-spec.md`, design in `aio-board-design.md` and
`aio-board-pinmap.md`, part selection in `aio-board-components.md`. This
file only tracks where the execution stands and what is open. Last
updated 2026-08-16.

## Where things stand

- **Project libraries** (`hardware/lib/`): 2 symbols (LSM6DSR, SPA06-003)
  and 4 footprints (SOIC-8 208 mil for the W25Q128JVSIQ, MLT-8540 buzzer,
  SWPA5040S inductor, Goertek LGA-8) created from the datasheets, every
  dimension cited, visually reviewed. The official KiCad LGA-14 footprint
  was verified usable as-is for the LSM6DSR (numeric comparison vs
  DS11976 Fig. 26).
- **Schematic** (`hardware/mark4-fc/`): COMPLETE. Root + 7 sheets (Power,
  MCU, Sensors, Storage, USB, Connectors, Indicators), 71 references.
  Verification gates passed: MCU pins checked 64/64 by script against
  `aio-board-pinmap.md`; root net merges 30/30; ERC 0 errors / 0 warnings
  with exactly 2 documented GUI exclusions (U4 SDx/SCx tied to GND per
  SNS-1). Every assembled part carries LCSC + MPN properties.
- **BOM**: fully sourced, 32 assembly lines / 64 assembled parts, zero
  missing LCSC refs (intentional no-BOM items: SWD header J1, BOOT0
  jumper JP1, test points TP1-TP5). Selections and rejections recorded in
  `aio-board-components.md` ("Secondary parts closed out" section).
- **PCB** (`mark4-fc.kicad_pcb`): 4 copper layers, JLCPCB-capable design
  rules (0.127 mm clearance/track, 0.3/0.45 via), outline 48 x 48 mm
  rounded, M3 mounting holes on the 30.5 x 30.5 pattern (3.2 mm NPTH,
  grommet keepouts). All 71 footprints placed per SNS-4/SNS-5/MEC-2
  (IMU dead center, axes aligned; baro far from the regulators; power
  block at the ESC connector; connectors on edges, openings outward).
  Courtyard overlaps: 0. NOT ROUTED yet.
- **Fab package** (`hardware/mark4-fc/exports/jlcpcb/`): gerbers zip +
  JLC-format BOM and CPL. Quote-grade (unrouted board is fine for
  pricing); NOT production-grade until routed and rotation-checked.
- **JLCPCB quote (2026-08-15)**: ~200 EUR for 5 boards, Standard PCBA
  (Economic refused: per-part "Standard Only" flag on the MEMS sensors,
  as SRC-1 anticipated). Cost structure: components 97 EUR (F722 alone
  43.50), feeder fees 42 (1.33/line, ALL classes pay on Standard),
  setup + stencil 29, X-ray 14 (LGA parts), PCB + color 14. Key insight:
  panel and size surcharges were ZERO - board size does not drive cost,
  the size decision is purely mechanical.

## Decisions taken

- 2026-08-15: hierarchical schematic (root + sheet per block), net name
  contract SPI1_*/UART*_TX/RX/M1-M4/etc. frozen by the MCU sheet.
- 2026-08-15: VCAP_1 (C16) = plain 4.7 uF X5R MLCC (C1779), knowingly
  departing from the DS11853 Table 19 ESR window. Evidence and risk
  statement in the schematic note and in `aio-board-components.md`.
- 2026-08-15: secondary parts closed out (LEDs red C2286 + yellow
  C89811 at 1k, R17 6.8 -> 5.1 ohm C17724, C12=C16 C1779, C15 C15849,
  load caps C1685, ADC clamp BAT54W C7502705). All Basic or
  Preferred-Extended: zero added feeder fees on Economic terms.
- 2026-08-16: MCU stays STM32F722RET6 (C118207). Alternatives study
  (JLC API + ST datasheets): only F722/F730/F732 exist in F7 LQFP64;
  F730R8T6 is a true drop-in (0 pin/AF diff, same RM0431) at -20% but
  64 KB flash hard ceiling and volatile stock; F4 fallbacks cost a pin
  rotation (no PC5 on F7 LQFP64!), a second VCAP and doc rework for
  10-17 USD per 5 boards - not worth it. Hedge: order 10 pcs
  (qty tier ~8.6 USD/pc).

## Open topics

1. **Placement / size review by Theo** (in progress, GUI-side). 48 x 48
   costs nothing extra at JLC; the constraint that forced it is the JST
   depth (6.65 mm) vs grommet keepouts, not area. Hand-soldering the
   JSTs (DNP) unlocks ~40 x 40 if the frame demands it.
2. **Cost trims to decide** (~-20 EUR total): green mask (-7), JSTs as
   DNP hand-soldered (-10), buzzer DNP (-2.5), R11/R12 5.1k -> 4.7k
   merge (-1.3; USB Rd tolerance 4.59-5.61k allows it).
3. **Routing** (after placement freeze): USB 90 ohm pair, crystal guard
   ring, SPI buses, GND/3V3 pours, DShot lines, DRC to JLC rules.
4. **CPL rotation check** against the JLC 3D preview before any real
   order (JLC zero-rotation differs from KiCad on SOT/SOIC/diodes/LEDs/
   USB-C; 13 refs listed in the fab-package report). Irrelevant for
   quotes.
5. **Doc follow-ups**: clarify MEC-1 wording (30.5 board vs 30.5
   pattern); soften the MCU-1 I2C BUSY-erratum rationale (that specific
   erratum is off the current F4 errata sheets; the v1-IP recovery
   concern stands); freeze the GPS connector J4 pin order (chosen at
   schematic time: GND, 3V3, TX, RX, SCL, SDA - not spec-frozen) before
   crimping; check board-side TX/RX naming against the mark1 cables.
6. **ES0360 errata** full read for the box-tick (summary shows no
   VCAP/regulator item; not verified page by page).
7. **MCU purchase hedge**: C118207 price moved 4.16 -> 10.04 USD in
   three days (stock-tier artifact, likely); decide order timing.

## Tooling notes

The board is drawn with KiCad 10 driven by a MCP server
(KiCAD-MCP-Server v2.6.0); its known bugs and workarounds (power-symbol
description corruption, missing T-junctions, .kicad_pro clobbering on
save_board, broken netlist tools on GUI-resaved files) are tracked in
the assistant session memory, with fixer scripts kept out of the repo.
Working rule: KiCad GUI closed while the tooling writes; whoever edits
last owns the file.
