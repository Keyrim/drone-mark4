# mark4-fc rev A - status and open topics

Living tracking document for the AIO flight controller board. Requirements
live in `aio-board-spec.md`, design in `aio-board-design.md` and
`aio-board-pinmap.md`, part selection in `aio-board-components.md`. This
file only tracks where the execution stands and what is open. Last
updated 2026-08-18.

## Where things stand

- **2026-08-18: board down to 36 x 36, SWD to 3 positions, corner bug**.
  Four changes in one pass, schematic and PCB both propagated, exports
  regenerated.
  1. **The corner "extra holes" were a contour bug**. Theo saw four
     bites taken out of the corners on the 3D preview. The four
     Edge.Cuts fillets each had their arc midpoint diametrically
     opposite where it belonged (arc (80,118)->(82,120) with a midpoint
     at (83.41,116.59) instead of (80.59,119.41)), so KiCad drew the
     270 degree arc instead of the 90 degree one and each corner lost a
     4 mm disc. All four were wrong the same way. The outline is now
     rebuilt from scratch: 36 x 36 centred on (100,100), four segments
     and four correct r = 2 fillets.
  2. **SWD goes from 6 to 3 positions** (MCU-2): SWDIO / SWCLK / GND,
     `PinHeader_1x03_P2.54mm_Vertical`. 3V3, NRST and SWO are gone; the
     SWO label on PB3 is replaced by a no-connect, so unconnected pins
     go 14 -> 15 and the net count stays at 81. That is what makes MEC-2
     hold on 36 mm: SWD plus USB-C need 19.4 mm of the 25.2 mm of edge
     that the two grommet keepouts leave, where the 1x06 needed 27.
     Price: connect-under-reset is gone (BOOT0 remains the recovery
     path) and the mark1 `Prog` 1:1 cable no longer fits.
  3. **36 x 36** (MEC-1), which is the floor this mounting pattern
     allows: a hole 15.25 mm off centre plus its 2.65 mm grommet
     keepout reaches 17.9 mm. Area drops 19 %, and the mounting holes
     keep 1.15 mm of material to the edge.
  4. **The bottom face takes the hand-soldered parts** (MEC-6, SRC-1
     assembles the top only): U2 (LDO), U6 (flash), Q1, JP1, TP1-TP5,
     and with them everything that serves them - D6/R15/R16/R17 with
     the buzzer, R8/R9/R10 with the flash, R3 with BOOT0. The rule is
     worth keeping: a pull-up that stays behind ends up 20 mm from the
     pin it holds. Top side courtyard drops 1143 -> 892 mm2 (69 % of
     the board), bottom rises to 415 mm2 (32 %).
- **2026-08-18 placement**: the 40 x 40 arrangement was shrunk 0.9
  toward the centre and relaxed (separation with annealed attraction),
  which preserves the reviewed topology, but it destroys exactly what
  matters most - a first pass left the crystal 11 to 15 mm from its
  pins. So the electrically load-bearing parts are hand-anchored and
  the relaxation only fills around them: crystal group (Y1 + C18/C19),
  VCAP, the buck bootstrap, the VBUS cap and the ESD array at the
  connector, one decoupling cap per MCU supply pin, and the whole buck
  and mux corner. Result: 0 courtyard overlap, and the critical nets
  come back in range - OSC_IN 11.2 -> 5.1 mm, OSC_OUT 14.8 -> 6.0,
  VCAP 6.7 -> 2.2, bootstrap 2.6. What unblocked the layout was moving
  the three LED strip pads off the inner row along the USB edge and
  onto the left edge (CON-2 shifted up one slot): that row was the
  congestion point, not the total area.
  DRC: 0 errors, schematic parity 0, 213 unconnected items (nothing is
  routed yet), and 79 silkscreen warnings which are all pad labels
  crossing a neighbour - the MEC-5 caveat, to be judged by eye.
- **2026-08-18 silkscreen and exports**. Every reference designator and
  value on the top silk is hidden (76 texts): what remains is the pad
  labels, which ARE the pads' identity, plus the MEC-4 orientation
  arrow, which did not exist until now. The arrow points +x, i.e.
  toward the CON-1/CON-3 edge, on the assumption that the GPS group
  (CON-2, left edge) faces the rear of the drone - CONFIRM before the
  frame is chosen, it is the one thing here nobody has verified.
  The fab package is regenerated, and it is now smaller on purpose:
  CPL 63 -> 52 placements, JLC BOM 31 -> 26 lines, because the bottom
  side is hand-soldered and must not reach JLCPCB. The parts that left
  the fab package (U2, U6, Q1, D6, R3, R8, R9, R10, R15, R16, R17) have
  to be bought separately, which also removes their Extended feeder
  fees from the quote.
- **2026-08-18 flash sizing: the STO-2 substitution note was wrong**.
  It offered "W25Q256JVSIQ (32 MB, same footprint)". That order code
  does not exist: Winbond never shipped the 256 Mbit die in SOIC-8
  208 mil. The real drop-in on the current land pattern is the Puya
  PY25Q256HB-SUH-IT (C50201614, ~$2.55 vs $1.76, Extended, 635 in stock
  on 2026-08-18 - thin, re-check at order time), with the Boya
  BY25Q256FSSIG (C22391877) as fallback and the Macronix MX25L25645GM2I
  (C190799, 54k in stock) as the always-available one, that last with
  RESET# instead of HOLD# on pin 7. Every 256 Mbit part crosses the
  24-bit address limit, so firmware owes an EN4B plus 4-byte commands;
  that is the real cost, not the 0.60 dollars. 64 MB has no credible
  option: no 512 Mbit SPI NOR exists in any 8-pin SOP at JLCPCB, the
  cheapest with stock is 4.3x the price in WSON-8.

- **2026-08-17 CPL rotations: real bug, found by eye, then measured**.
  Theo saw on the JLC preview that the SOT-223 LDO (U2) sat 180 degrees off
  - on this revision and on the previous one, so it had never been right.
  Cause: KiCad's footprint orientation does not match the orientation of
  JLCPCB's own part library, and the CPL was exporting the raw KiCad
  rotation.
  The community table (matthewlai/JLCKicadTools) explains U2 exactly
  (`^SOT-223 -> 180`) and pointed at six more parts. But a second preview
  pass killed the idea of trusting it blindly: **it is wrong for two of our
  nine mapped parts** - TSOT-23-6 (U1, the buck: table says 180, actual need
  is 270, which is what Theo read off the preview as "not half a turn, a
  quarter") and SOT-23 (Q1: table says -90, actual need is 180, and nobody
  had spotted that one because a 3-lead package turned 90 degrees is not
  obvious on a preview).
  So the corrections were **measured** instead, by script: fetch the real
  EasyEDA footprint of each LCSC reference from
  `easyeda.com/api/products/<LCSC>/components`, compare it with the KiCad
  footprint in use, twice over (centroid-to-pad-1 angle, and the angle of
  the vector between the two farthest common pads). Both methods agree on
  every part with residuals under 5 degrees, and they reproduce the three
  corrections already confirmed visually.
  **The table is now keyed by LCSC reference, not by footprint family**
  (`jlcpcb-cpl-rotations.csv`), because the measurements proved that is the
  right index: D4 (C2286) and D5 (C89811) are both LED_0603 on the same
  KiCad footprint and need 180 and 0 respectively - two manufacturers, two
  footprint orientations. A footprint-keyed table cannot express that. The
  community table is kept underneath as a fallback for newly added parts,
  explicitly marked as a guess to be measured.
  11 corrections now applied: U1 270, U2 180, U3 270, U4 180, U5 90, U6 270,
  U7 270, U8 270, Q1 180, D4 180, J2 180. Measured as needing nothing: D3,
  D5, D6, Y1, L1. The only parts with no rule at all are the 0603/0805
  resistors and capacitors, which are symmetrical.
  One exception documented in the file: **J2, the USB-C receptacle, is the
  one value not measured**. Its EasyEDA footprint merges and probably swaps
  the A/B rows of this reversible connector, so the automatic comparison
  returns 0 while the preview clearly shows 180 is right (the opening faces
  the board edge). Re-check that one by eye on any respin.
- **2026-08-17 (second pass): external IO becomes individual pads**.
  The five 1xN pad groups (J3/J4/J5/J6/J8) are replaced by **27
  individually placed pads** (J10-J13 CON-1, J20-J26 CON-2, J30-J33
  CON-3, J40-J48 ESC-3, J50-J52 IND-3), all on one new footprint
  `SolderPad_1x01_THT`, whose silkscreen label is the component VALUE
  drawn vertically on both faces - so each pad names its own signal
  (`TX1`, `M1`, `VBAT`, `LED5V`, ...) with a single footprint to
  maintain. Reason: the groups reserved 66.4 mm of board edge in five
  indivisible blocks, which on a 40 x 40 outline forced the
  distribution over the four sides; individual pads remove that
  constraint entirely, and a second row is usable since the holes are
  plated and labelled on both faces (back row soldered from below).
  Courtyard drops from 139 to 119 mm2, which is the small part of the
  gain.
  Unexpected side benefit, worth more than the area: **the ESC pads no
  longer have an ORDER**, so the double numbering that the docs called
  the most likely wiring mistake on this board (ESC-3 spec order versus
  the J7 Velox order) simply ceased to exist. Each pad states its
  signal; only J7 keeps a numbering.
  Verification: ERC 0/0, netlist diffed pad by pad against the
  pre-change baseline (81 -> 81 nets, every old `Jx.n` replaced by the
  right `Jyy.1` on the same net, no net gained or lost), BOM unchanged
  (all pads `in_bom no`), 99 components, ASCII clean, the three pad
  blocks reviewed on the rendered PDF.
- **2026-08-17: no TVS added on VBUS**, after examining it. The
  USBLC6-2SC6 already on the board protects VBUS as well as the data
  lines (VBR 6.1 V, clamps 11-13 V at 8 A), and no passive part can
  hold 5.25 V and cap at the mux's 6 V absolute maximum - the window is
  14 %, a TVS needs about a factor of two. A discrete 5 V TVS would
  only move the clamp to ~9.8 V on a risk already covered; the case it
  would not cover either (sustained overvoltage) needs an active OVP
  switch. Cost was never the issue: ~1.55 EUR on the 5-board lot.
  Reasoning kept in PWR-3 so it does not get re-litigated.
- **2026-08-17 power and IO update: docs and schematic done, PCB NOT**.
  Three changes, all propagated to the schematic, the BOM and the
  exports on 2026-08-17:
  1. **Active OR-ing** (PWR-3). D1/D2 (SS54 pair) removed, replaced by
     U8 TPS2116DRLR (C3235557) in SOT-583, plus R18/R19 = 100k/33k on
     PR1 and R20 = 10k pulling up ST. The shared 5 V rail goes from
     ~4.6-4.7 V to ~4.98 V, and the diode heating that capped rail
     current is gone. MODE tied to VIN1 = priority mode, so the BEC
     always wins and USB can be plugged or unplugged under power.
     C26 (a third 22 uF) added on the mux output because switchover is
     break-before-make. New net `+5V_BUCK` (project power symbol,
     cloned from `power:+5V`) for the buck output ahead of the mux; it
     needed its own PWR_FLAG (#FLG06) and the old +5V flag pair
     (#FLG02/#PWR07) had to go, since U8 VOUT now drives that rail.
     ST reaches the MCU on PC12 through the new `PWR_SRC` net.
     D1/D2 stay RETIRED designators - no renumbering, D3..D6 keep
     theirs.
  2. **GPS pad group goes to 7 pads** (CON-2; the group itself was
     dissolved into individual pads a few hours later, see the entry
     above - what survives is the 5 V and the seventh pad). J4 was
     `SolderPads_GPS_1x07_P2.54mm_THT`, order GND / 5V / TX / RX / SCL
     / SDA / 3V3, and pad 2 changed from 3V3 to 5V: the reference
     module (Matek M9N-5883) wants 4-6 V on its own LDO, like every
     commercial GPS. The two supplies sit at opposite ends on purpose
     (a 5V-to-3V3 bridge kills the 3V3 rail; a 3V3-to-SDA bridge only
     stalls a bus). Costs 2.54 mm more board edge: 17.34 vs 14.80.
  3. **Addressable LED strip pads** (new IND-3; also dissolved into
     individual pads the same day). J8, 3 pads
     (GND / +5V_BUCK / DATA) on `SolderPads_LED_1x03_P2.54mm_THT`,
     R21 = 330R in series, data from PA10 = TIM1_CH3 (AF1) on DMA2
     S6C6 - verified against `STM32F722RE.json`, not assumed. Its 5 V
     comes from `+5V_BUCK`, upstream of the mux, so strip current never
     crosses the mux and the strip stays dark on USB alone. No level
     shifter: 3.3 V into a WS2812 is marginal per datasheet, standard
     practice everywhere, and the fallback (a diode in the first LED's
     supply) needs no respin.
  Verification passed: ERC 0/0 (the 2 IMU strap exclusions still in the
  `.kicad_pro`), netlist diffed net by net against a pre-change
  baseline (79 -> 81 nets, only the intended changes), unconnected pins
  16 -> 14 (PA10 and PC12 consumed), BOM 30 -> 32 assembly lines and
  60 -> 64 pieces, ASCII clean, every edited file re-parsed. Exports
  regenerated (BOM, ERC rpt + json, 8-page PDF), the mux and the two
  pad groups reviewed visually on the rendered PDF.
  **The PCB is still untouched** and now also lags these three changes,
  on top of the 2026-08-16 backlog below.
- **2026-08-16 spec update: schematic and BOM propagated, PCB NOT**.
  Done in KiCad on 2026-08-16: 10 V BEC input rail (`+12V` net renamed
  `+10V`), VBAT sense (new `VBAT` net, `12V_SENSE` renamed
  `VBAT_SENSE`, divider moved and resized), all JST-SH replaced by
  solder pad groups (J3/J4/J5/J6, 3 new footprints). Still pending:
  the PCB and the fab package, which keep the old JST footprints, the
  48 x 48 outline and the old placement, and do not yet apply the new
  directives MEC-2/4/5/6. The PCB and fab-package entries below
  predate the update.
- **Project libraries** (`hardware/lib/`): 4 symbols (LSM6DSR,
  SPA06-003, VBAT and, since 2026-08-17, +5V_BUCK - the stock KiCad
  power library has neither VBAT nor a pre-mux 5 V, and both are clones
  of a real library definition rather than hand-made symbols, which is
  what keeps ERC quiet) and 5 footprints (SOIC-8 208 mil for the
  W25Q128JVSIQ, MLT-8540 buzzer, SWPA5040S inductor, Goertek LGA-8 and
  the single solder pad below) created from the datasheets, every
  dimension cited, visually reviewed. The official KiCad LGA-14
  footprint was verified usable as-is for the LSM6DSR (numeric
  comparison vs DS11976 Fig. 26).
- **Solder pad footprint**: `SolderPad_1x01_THT`, one pad, plated
  through-hole, 0.8 mm drill / 1.6 mm pad, courtyard 2.10 x 2.10 mm,
  `${VALUE}` label vertical on F.SilkS and B.SilkS (mirrored),
  `exclude_from_pos_files` + `exclude_from_bom`. It replaces the five
  1xN group footprints of 2026-08-16/17 (`SolderPads_UART_1x04`,
  `_GPS_1x06`, `_GPS_1x07`, `_ESC_1x09`, `_LED_1x03`), all deleted the
  same day without ever being committed. Courtyard is copper + 0.25 and
  deliberately excludes the label, as in the groups it supersedes: DRC
  will NOT protect label legibility, and now that the label IS the
  pad's identity, checking it by eye matters more than before (MEC-5).
  Design history worth keeping, because it constrains placement: the
  1xN groups had settled on a 2.54 mm pitch, not 2.00, because 2.00
  with a 1.6 mm pad leaves only 0.3 mm of copper between neighbours,
  too tight to hand-solder comfortably (2.54 leaves 0.94 mm). That is
  now a PLACEMENT rule rather than a footprint property: keep 2.54 mm
  between neighbouring pads unless there is a reason not to. Reference
  for the whole approach: T-Motor F7 (F722). Four rounds were drawn and
  discarded before this one: THT at 2.00 pitch, SMD lands, THT groups at
  2.54, then the 1x07 GPS variant.
- **ESC connector J7** (2026-08-16, ESC-4): JST SM10B-SRSS-TB
  (C160409, ~7k stock, $0.198), stock KiCad footprint
  `JST_SH_SM10B-SRSS-TB_1x10-1MP_P1.00mm_Horizontal`, wired in
  parallel with the J40-J48 pads at the measured Velox wire order so the
  harness plugs straight in. Marked `in_bom no`: it is expected to sit
  on the bottom face, where SRC-1 (JLCPCB assembles the top only)
  makes it a hand-soldered part. Revisit at placement time - if it
  goes on the top face it can be assembled, for one feeder fee.
  Between 2026-08-16 and 2026-08-17 this connector had a twin hazard:
  J7 was in Velox order while the J6 pad row was in ESC-3 spec order,
  two numberings for the same signals, called out then as the most
  likely wiring mistake on this board. Individual pads killed it - the
  ESC pads have no order left, each states its own signal, and only J7
  is numbered. Place those pads in Velox order and the two paths read
  alike.
  Courtyard is copper + 0.25 mm, deliberately NOT enclosing the silk.
  Consequence for placement: DRC will
  NOT protect label legibility, a neighbour may legally overlap the
  labels, so check it by eye (MEC-5).
- **Schematic** (`hardware/mark4-fc/`): COMPLETE. Root + 7 sheets (Power,
  MCU, Sensors, Storage, USB, Connectors, Indicators), 98 references /
  99 components (71 before the 2026-08-17 updates; the jump is the 27
  individual pads replacing 5 pad groups).
  Verification gates passed: MCU pins checked 64/64 by script against
  `aio-board-pinmap.md`; root net merges 30/30; ERC 0 errors / 0 warnings
  with exactly 2 documented GUI exclusions (U4 SDx/SCx tied to GND per
  SNS-1). Every assembled part carries LCSC + MPN properties.
- **BOM**: fully sourced, 32 assembly lines / 64 assembled parts, zero
  missing LCSC refs (intentional no-BOM items: SWD header J1, BOOT0
  jumper JP1, test points TP1-TP5, and the 27 individual solder pads
  J10-J52). Was 30/60 between the 2026-08-16 pads decision and
  the 2026-08-17 power update, and 32/64 before that - same totals,
  different content. Selections and rejections recorded in
  `aio-board-components.md` ("Secondary parts closed out" section).
- **PCB** (`mark4-fc.kicad_pcb`): rebuilt 2026-08-17 from the current
  netlist. 4 copper layers, JLCPCB-capable design rules (0.127 mm
  clearance/track, 0.3/0.45 via) and the M3 grommet holes carried over
  from the previous board; **outline now 40 x 40 mm** rounded (r = 2),
  103 footprints placed, DRC clean apart from the 216 unconnected pads
  that an unrouted board is expected to have.
  **The placement is a quick automatic pass, not a design.** It was done
  to answer one question - what size closes - and it does: 40 x 40 holds
  71 % courtyard fill on the front face, 38 and 39 mm do not close at
  all. Honoured: IMU dead centre (0.000 mm offset), axes aligned, baro
  in the opposite corner from the buck and the inductor, USB-C on the
  bottom edge with its opening outward, buzzer and the J7 receptacle on
  the back face (MEC-6, which is what let the outline reach 40).
  Known defects to fix when the placement is done by hand: the 3V3 LDO
  drifted to 8.6 mm from the baro (the right-hand quadrant filled up, so
  the placer pushed it left); the SWD header sits 2.9 mm inside the
  bottom edge rather than on it; several refdes and pad labels overlap
  their neighbours (MEC-5). NOT ROUTED.
  Geometry found while doing it, worth keeping: **below 42 mm the edge
  pad lane collides with the M3 grommet keepout**, which spans 12.65 to
  17.85 mm from the centre on both axes - so an edge row has to dodge
  the four hole zones, and that is what pushed the LED pads from the
  right edge to the bottom one. And **the SWD footprint cannot sit on
  the same edge as the USB-C at 40 mm**: between the grommet keepouts a
  board edge offers 25.3 mm, while USB-C plus a 6-position SWD needs
  27 mm. Shrinking SWD to 3 positions (doc follow-up 7) buys 7.6 mm and
  would close it.
- **Fab package** (`hardware/mark4-fc/exports/jlcpcb/`): regenerated
  2026-08-17 for the 40 x 40 board - gerbers zip (16 files: 4 copper,
  masks, pastes, silkscreens, profile, PTH + NPTH Excellon with maps),
  JLC-format BOM (31 lines / 63 pieces) and CPL (63 positions).
  BZ1 is absent from both on purpose: it is on the back face, which
  SRC-1 does not assemble, so it is hand soldered like J7 - which is
  also why the assembled line count went 32 -> 31 versus the schematic
  BOM. Quote-grade (an unrouted board prices fine); NOT
  production-grade until routed, and until the four remaining unmapped
  footprints are checked on the preview (see the CPL rotation entry
  above); the other eight rotations are now corrected automatically.
- **JLCPCB quote (2026-08-17, this revision)**: **173.59 EUR for 5 boards**,
  Standard PCBA, against ~200 EUR on 2026-08-15. Structure: components
  80.54 (31 lines), feeder loading 40.95 (1.32 per line), setup 22.07,
  X-ray 14.16, stencil 7.09, bare PCB 6.04, SMT assembly 2.33, packaging
  0.42. Where the ~26 EUR went: about 11 of it is the MCU, which fell from
  8.70 EUR/pc at the last quote to 7.60 USD/pc today (C118207, checked
  2026-08-17); the bare PCB dropped 14 -> 6.04, but that is mostly the
  green mask trim rather than 48 x 48 -> 40 x 40, which confirms the old
  finding that board size costs nothing here. Cost shape worth keeping:
  **70.53 EUR of this is fixed** (feeders + setup + stencil + packaging,
  41 % of the total) and 20.61 EUR per board is variable, so 10 boards
  would land near 276 EUR, i.e. 27.6 EUR each against 34.7 at five.
- **JLCPCB quote (2026-08-15, previous revision)**: ~200 EUR for 5 boards, Standard PCBA
  (Economic refused: per-part "Standard Only" flag on the MEMS sensors,
  as SRC-1 anticipated). Cost structure: components 97 EUR (F722 alone
  43.50), feeder fees 42 (1.33/line, ALL classes pay on Standard),
  setup + stencil 29, X-ray 14 (LGA parts), PCB + color 14. Key insight:
  panel and size surcharges were ZERO - board size does not drive cost,
  the size decision is purely mechanical.

## Decisions taken

- 2026-08-17: 5 V OR-ing becomes an active power mux (TPS2116) instead
  of two Schottkys. Reason: the GPS module picked for CON-2 needs 4-6 V
  and the Schottky pair left the rail at ~4.6 V falling to ~4.4 V under
  load, and a diode pair was going to be the current ceiling of the
  rail as soon as an ESP32 or LEDs joined. Reverses the 2026-08-12
  "ideal-diode ICs rejected" note, which had only looked at the
  single-input LM66100 and concluded no 2-input part existed under
  $0.50; the TPS2116 is that part at $0.45. Accepted in exchange: an
  active part in the flight power path, a 6 V absolute maximum on a
  node that used to tolerate anything, a 0.35 mm pitch package that
  cannot be reworked by hand, and a break-before-make gap that the
  output bulk has to ride. Fallback kept on the shelf: 2x LM66100DCKR
  in SC-70-6, hand-solderable.
- 2026-08-17: CON-2 exposes both 5 V and 3V3 on 7 pads rather than
  choosing. The 3V3-only group was frozen before a GPS existed and
  would have run a Matek M9N-5883 below its LDO dropout.
- 2026-08-17: the LED strip gets real hardware now (IND-3), not a note
  for later, and its supply is taken ahead of the mux. Cost: one
  GPIO (PA10), one timer channel (TIM1_CH3) and one DMA stream
  (DMA2 S6C6). TIM1 was only ever excluded for the four motor
  channels, because CH4 lands on USB_DM; one channel was always free.

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
- 2026-08-16: the ESC is the T-Motor Velox 45A V2, harness measured
  (JST-SH 1 mm, 10 wires: GND, GND, VBAT, 10V, M4, M3, M2, M1, CRT,
  TX). Board input becomes the 10 V BEC (PWR-1, never raw VBAT:
  braking spikes vs the AP63205 32 V rating); the PWR-4 divider moves
  to VBAT, resized for 25.2 V / 6S - battery voltage is now directly
  measurable, which the regulated-rail design could not do.
- 2026-08-16: all JST-SH connectors dropped for solder pads, ESC
  included (the 4-in-1 pinout is not standardized across brands, so a
  frozen connector pinout buys nothing). Kills the JST-depth
  constraint behind 48 x 48; new outline target 40 x 40. Only USB-C
  and the SWD through-holes remain as mechanical connectors.
- 2026-08-16: placement directives frozen in the spec: SWD and USB-C
  on the same edge facing a lateral side of the drone (MEC-2),
  silkscreen orientation arrow (MEC-4), refdes only where readable
  (MEC-5), bottom side available (MEC-6).
- 2026-08-16: PWR-4 divider resized for the move to VBAT. R1/R2 go
  from 10k/1.5k to **100k/12k** (C25803 / C22790, same UNI-ROYAL
  0603WAF family as the rest of the board, so no new part class).
  Ratio 12/112 = 0.10714: 2.70 V at 25.2 V (6S max), 1.80 V at 16.8 V
  (4S max), VDDA 3.3 V only reached at 30.8 V. The old 10k/1.5k would
  have sat at 3.29 V at 25.2 V, i.e. no margin at all under VDDA.
  Drain 0.22 mA while the pack is plugged, against 2.25 mA for a
  10k/1.2k of the same ratio. Source impedance 10.7 k is fine: C7
  sits at the pin and the conversion runs at about 1 Hz with a long
  sampling time. D3 (BAT54W) still holds: its 2 uA at 25 V is a
  worst case at full reverse bias, and this node never exceeds 2.7 V.
- 2026-08-16: the ESC pad order follows the ESC-3 spec list (GND, VIN,
  VBAT, M1..M4, CURR, TLM) and deliberately does NOT match the
  measured Velox wire order, so five wires cross at soldering time.
  The pad labels are the pinout; verify with a continuity check.

## Open topics

1. ~~Propagate the 2026-08-16/17 decisions into the PCB~~ DONE
   2026-08-17: the board was rebuilt from the current netlist, 40 x 40,
   DRC clean, fab package regenerated and quoted. See the PCB entry
   above for what the automatic placement got right and what it did not.
2. ~~Placement by hand~~ DONE 2026-08-18 on the 36 x 36 (see the
   placement entry above): MEC-2 holds, MEC-4 now has an arrow, MEC-5
   is applied (refdes hidden, pad labels kept), MEC-6 carries the
   hand-soldered set. Of the three defects listed for the 40 x 40, the
   LDO/baro distance is fixed by construction (U2 is on the bottom,
   17 mm from U5) and the SWD now sits at the edge; what is left is
   silkscreen readability, which DRC cannot judge.
   What still needs an eye or a decision:
   - the 79 silkscreen warnings are pad labels crossing a neighbour's
     pads or outline, which the fab will clip. The ESC row is the worst
     case since the MCU starts 1.2 mm below it. Judge on the plot, and
     if it is not good enough, shrink the label font in
     `SolderPad_1x01_THT` rather than move pads;
   - the MEC-4 arrow direction is an assumption (GPS group to the rear,
     so forward is +x). Confirm it against the frame;
   - the ESC pads are still in the order the 40 x 40 left them; check
     they read like J7 in the Velox order before soldering.
   Placement rules that stay true for any later change: keep 2.54 mm
   between neighbouring pads (below that, 1.6 mm pads leave under
   0.3 mm of copper and hand soldering gets unpleasant), keep the pads
   of one interface together and in a sensible order, never put the
   CON-2 5V and 3V3 pads side by side, and remember every pad is plated
   through all four layers, so a row of them is a line of holes through
   both inner planes: plan pour clearances and thermals around them.
   Edge capacity, measured on 36 mm: 9 pad slots per edge between the
   grommet keepouts, i.e. 27 for the three edges the USB does not take,
   which is exactly the pad count - the LED group only fits because
   CON-2 was shifted one slot up on the left edge.
3. **Frame replacement** (Source One V3: pressed inserts popped out
   of the carbon). At 36 x 36 any standard 5-inch 30.5-pattern frame
   fits, with more clearance than before; remaining criteria: open CAD for printed mounting parts
   (proven need), crash tolerance, stack height, and a clear lateral
   face for the USB/SWD edge (MEC-2). Re-buying a Source One V3 with
   printed captive-nut fixes is a candidate.
4. **Cost trims to decide**: green mask (-7), buzzer DNP (-2.5),
   R11/R12 5.1k -> 4.7k merge (-1.3; USB Rd tolerance 4.59-5.61k
   allows it). The JST-DNP trim is superseded: they are gone entirely
   (~-10 EUR plus 3 feeder fees).
5. **Routing** (after placement freeze): USB 90 ohm pair, crystal guard
   ring, SPI buses, GND/3V3 pours, DShot lines, DRC to JLC rules.
6. **CPL rotation check** against the JLC 3D preview before any real
   order (JLC zero-rotation differs from KiCad on SOT/SOIC/diodes/LEDs/
   USB-C; 13 refs listed in the fab-package report). Irrelevant for
   quotes.
7. **Doc follow-ups**: ~~clarify MEC-1 wording~~ done 2026-08-18, it
   now states the 36 x 36 outline and why that is the floor; soften the MCU-1 I2C BUSY-erratum rationale (that specific
   erratum is off the current F4 errata sheets; the v1-IP recovery
   concern stands); check pad-side TX/RX label conventions against the
   mark1 cables before soldering (pad order is moot now that pads are
   individual, but the TX/RX naming convention still has to match the
   cables).
8. **ES0360 errata** full read for the box-tick (summary shows no
   VCAP/regulator item; not verified page by page).
9. **MCU purchase hedge**: C118207 price moved 4.16 -> 10.04 USD in
   three days (stock-tier artifact, likely); decide order timing.

## Tooling notes

The board is KiCad 10, but it is no longer driven through the MCP
server (KiCAD-MCP-Server v2.6.0): its bugs land on the critical path of
a schematic edit (power-symbol description corruption, missing
T-junctions, .kicad_pro clobbering on save_board, broken netlist tools
on GUI-resaved files). Since 2026-08-17 the schematic is edited
directly in s-expression and the PCB through `pcbnew` in the system
python3, with the scripts kept out of the repo. Verification gates that
caught real defects: `kicad-cli sch erc`, a netlist exported and diffed
net by net against a baseline taken before the edit, `kicad-cli pcb
drc` (its schematic-parity check is what proves the board still matches
the sheets), a courtyard overlap report, and a PDF plot rasterised with
`pymupdf` and read by eye - the plot is the gate that catches what no
rule checker sees. Working rule: KiCad GUI closed while the tooling
writes; whoever edits last owns the file.
