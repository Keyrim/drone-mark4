# mark4-fc rev A - component selection

Full parts list for the board specified in `aio-board-spec.md` (pin
assignments in `aio-board-pinmap.md`). The main sensors were selected in
`sensor-board-components.md`; this document adds the MCU-side and
secondary parts and consolidates everything the schematic needs.

Prices, stock and the Basic/Extended class were read on 2026-08-12 from
the JLCPCB parts API (the `componentLibraryType` field is authoritative;
rendered pages are not) and LCSC product pages. Basic parts cost no
feeder fee; every unique Extended reference adds one (~3 USD). Re-check
stock at order time - the volatile lines are flagged.

## Main parts (selection rationale in the docs cited)

| Role | MPN | LCSC | Price | Notes |
|---|---|---|---|---|
| MCU | STM32F722RET6 | C118207 | $4.16 | `aio-board-spec.md` MCU-1 |
| IMU | LSM6DSRTR | C784817 | $3.20 | `sensor-board-components.md`; wiring corrected there (aux pins NC) |
| Baro | SPA06-003 | C30589048 | $0.74 | same doc; SDO to GND via 10k |
| Blackbox flash | W25Q128JVSIQ | C97521 | ~$1.70 | STO-1 strapping: /CS, /WP, /HOLD 10k pull-ups. 32 MB alternative on the SAME land pattern: Puya PY25Q256HB-SUH-IT (C50201614, ~$2.55 at qty 5, Extended, 635 in stock on 2026-08-18 so re-check before ordering); fallback Boya BY25Q256FSSIG (C22391877, 1.5k in stock), always-in-stock option Macronix MX25L25645GM2I (C190799) but its pin 7 is RESET#, not HOLD#. Any of them needs EN4B plus 4-byte addressing in firmware - see STO-2 |
| 5 V buck | AP63205WU-7 | C2071056 | ~$0.40 | PWR-1; L/C per its Table 3, EN to VIN, BST 100 nF to SW |

## USB and power OR

| Role | MPN | LCSC | Price | Stock | Class | Notes |
|---|---|---|---|---|---|---|
| USB-C receptacle 16p USB2.0 | HRO TYPE-C-31-M-12 | C165948 | $0.19 | 446k | Extended | De facto JLC standard; no 16-pin USB-C exists in Basic at all |
| ESD array D+/D-/VBUS | ST USBLC6-2SC6 | C7519 | $0.18 | 31k | Extended | Flow-through pinout; all USBLC6 types are Extended, no saving in the clone |
| 5 V power mux (OR-ing) | TI TPS2116DRLR | C3235557 | $0.45 | 11.5k | Extended | Replaces the SS54 pair, 2026-08-17. 1.6-5.5 V, 2.5 A, RON 40 mOhm typical and 60 mOhm max over temperature: 24 mV of drop at 400 mA instead of ~300 mV, and reverse blocking in both legs (PWR-3). SOT-583, 0.35 mm pitch: fabricable by SRC-1 in Standard, but not reworkable by hand |
| Mux PR1 divider 100k + 33k 0603 | UNI-ROYAL 0603WAF1003T5E + 0603WAF3302T5E | C25803 + C4216 | $0.005 | 2.8M / 2.8M | Basic | Threshold VREF * 133/33 = 4.03 V on VIN1; the 100k is the same reference as the VBAT divider top resistor, so 33k is the only new line |
| Mux ST pull-up 10k 0603 | UNI-ROYAL 0603WAF1002T5E | C25804 | $0.005 | 2.8M | Basic | ST is open drain; existing 10k reference, no new line |
| CC pulldowns 2x 5.1k 0603 | UNI-ROYAL 0603WAF5101T5E | C23186 | $0.004 | 9M | Basic | One per CC pin, never shared; nothing else needed for a UFP-only port |

Alternates: TYPE-C-31-M-31 (C2760486, verify footprint before swapping).
Mux alternates, both Extended: 2x LM66100DCKR (C2869734, $0.25 each,
SC-70-6, 79 mOhm, CE of each tied to the other supply per the datasheet
ORing figure) - two packages instead of one, but a 0.65 mm pitch that a
soldering iron can reach, which is the reason to keep it on the shelf;
LM66200DRLR (C3235556, $0.49, dual in one SOT-583).
Correction to the 2026-08-12 entry, which claimed "no true 2-input OR
part under $0.50 at LCSC" and only considered the single-input LM66100:
the TPS2116 is exactly that part, at $0.45. The Schottky pair (SS54
C22452 Basic, or SS34 C8678) remains the fallback if the mux ever has to
go, at the cost of ~300 mV of rail and the diode heating.

## Clock and 3V3

| Role | MPN | LCSC | Price | Stock | Class | Notes |
|---|---|---|---|---|---|---|
| HSE 8 MHz 3225 | JSCJ CJ13-080000910B20 (CL 9 pF, ESR 120 max, +/-10/+/-20 ppm) | C712812 | $0.23 | 435 (THIN) | Extended | Only reputable part passing AN2867: gm_crit 0.175 mA/V vs the F722's 1 mA/V = margin 5.7 (>= 5 required). ppm worst case +/-55 vs +/-500 USB bound. No 8 MHz 3225 exists in Basic (all 173 refs checked) |
| Load caps 2x 8.2 pF C0G 0603 | 0603CG8R2C500NT | C1685 | $0.0093 | 74.9k | Preferred-Extended | C = 2 x (CL 9 - stray 5) = 8 pF -> 8.2 E12; trim 6.8/10 on bench if layout runs long. Closed out 2026-08-15, see that section |
| 3V3 LDO | TI TLV1117LV33DCYR (SOT-223) | C15578 | $0.32 | 1.3k | Extended | Only candidate with a GUARANTEED max dropout (455 mV at 1 A) covering the post-OR 4.4 V corner: 3.3 + 0.455 = 3.76 V << 4.42 V worst rail. LV variant is the ceramic-stable one - never substitute a classic 1117. 10 uF ceramic in AND out |

Rejected with numbers: YXC X32258MOB4SI (margin 2.9, fails AN2867),
AMS1117-3.3 (the only Basic LDO: dropout 1.1 V = zero guaranteed margin
at the corner, output would leave the 3.0-3.6 V USB PHY window).
Alternates: crystal huaxindianzi 3X008000BP (C19723344, 60 ohm claimed,
margin 7.3, no-name - pull the datasheet first); LDO ME6211C33M5G-N
(C82942, passes on typicals only) or MCP1826S-3302 (C638624, the mark1
part, fine at 5.6x the price).

## Buzzer, connectors, buck passives

| Role | MPN | LCSC | Price | Stock | Class | Notes |
|---|---|---|---|---|---|---|
| Buzzer (passive magnetic) | MLT-8540 | C95300 | $0.27 | 7.7k | Extended | 85 dB min at 10 cm; passive because PB5 = TIM3_CH2 makes tones free (IND-2 already pays the transistor) |
| Buzzer transistor | S8050 J3Y SOT-23 | C2146 | $0.02 | 654k | Basic | Low-side; 1k base resistor (C21190), deep saturation at the ~220 mA coil current |
| Flyback diode | 1N4148W SOD-123 | C81598 | $0.01 | 1.6M | Basic | Across the coil, cathode to 5 V |
| SWD header (CON-4) | through-hole 2.54 mm 1x3 footprint | none | - | - | not assembled | No BOM line: hand-populated from bench stock. Cut from 6 to 3 positions on 2026-08-18 (SWDIO/SWCLK/GND per MCU-2), which is what frees the edge for SWD and USB-C to share one side of a 36 mm board |
| Buck inductor 4.7 uH | Sunlord SWPA5040S4R7MT | C48496 | $0.08 | 4.1k (THIN) | Extended | Shielded 5x5x4; Isat 3.5 A min (>= 2.7 required), DCR 30 mOhm |
| Buck CIN 10 uF/25 V 0805 | Samsung CL21A106KAYNNNE | C15850 | $0.10 | 10M | Basic | |
| Buck COUT 2x 22 uF/25 V 0805 | Samsung CL21A226MAQNNNE | C45783 | $0.45 | 4M | Basic | |
| BST cap 100 nF 0603 | YAGEO CC0603KRX7R9BB104 | C14663 | $0.02 | 100M | Basic | SW-to-BST, not to GND |

Alternates: buzzer MLT-8530 (C94599, 1 mm lower, 7x stock, 80 dB min);
inductor SWPA4030S4R7MT (C57269, 4x4x3, Isat 2.9 A, 10x stock).

Dropped 2026-08-16: all three JST-SH connectors - UART x2 SM04B-SRSS-TB
C160404, GPS SM06B-SRSS-TB C160405, ESC SM08B-SRSS-TB C160407. External
IO moved to plated through-hole solder pads (`aio-board-spec.md`
section 7 and ESC-3); references kept here for the record.

Added back 2026-08-16, one only: the ESC receptacle of ESC-4, JST
SM10B-SRSS-TB(LF)(SN) C160409 ($0.198, ~7k stock, genuine JST, side
entry like the dropped ones). Ten positions, not eight: it matches the
measured T-Motor Velox 45A V2 harness plug exactly, so that harness
needs no soldering. It sits in parallel with the ESC-3 pads and is
NOT in the BOM - it is expected on the bottom face, which SRC-1 does
not assemble, so it is hand-soldered. If placement ever puts it on the
top face, it costs one feeder fee.

## Consolidated schematic notes

- OR topology (rewritten 2026-08-17): U8 TPS2116, VIN1 = buck output
  (`+5V_BUCK`), VIN2 = VBUS, VOUT = the shared `+5V` rail. MODE is tied
  to VIN1, which selects priority mode and makes the BEC win whenever it
  is present; R18/R19 (100k/33k) divide VIN1 into PR1 for a 4.03 V
  switchover threshold; ST goes to PC12 through a 10k pull-up. C26 (a
  third 22 uF, same reference as the buck output pair) sits on VOUT
  because the switchover is break-before-make: 8 us with only C5 would
  droop 457 mV at 400 mA, C5 + C26 hold near 145 mV. The buck-leg block
  is still mandatory (PWR-3, body-diode back-feed) and the mux provides
  it internally, which is why the Schottky pair could go.
- LED strip (IND-3): the LED5V pad (J51) hangs on `+5V_BUCK`, upstream of
  U8, so strip current never crosses the mux and the strip stays dark on
  USB alone; R21 330R (C23138, UNI-ROYAL 0603WAF3300T5E, Basic) in series
  on the data line from PA10.
- USB: shell tabs straight to GND plane; USBLC6 flow-through next to
  the connector, pin 5 on VBUS; 90 ohm pair to PA11/PA12, no series
  resistors; no VBUS divider (firmware forces B-valid).
- Crystal: guard ground around crystal + caps stitched to MCU VSS,
  loop to PH0/PH1 short, nothing under the body; caps NP0 only.
- LDO: 10 uF X5R in and out, SOT-223 tab on a 3V3 pour.
- Buzzer: low-side S8050, 1k base from PB5, optional 10k base-to-GND
  (silence while PB5 is high-Z at reset), 1N4148W across the coil. The
  8540 coil is a 3.6 V class part on a ~4.6 V rail: a 5.1 ohm series
  resistor is fitted (decided 2026-08-15, see the closeout section; also
  protects the coil from a firmware bug leaving PB5 high, which a duty
  cap would not).
- External IO as individually placed labelled pads (2026-08-17,
  supersedes the 1xN groups): one `SolderPad_1x01_THT` per pad, 27 of
  them, silkscreen label taken from the VALUE field. No BOM line and no
  cost either way - they were never assembled parts. SWD and USB-C
  share one lateral edge (MEC-2).
- ADC divider on PC0: from the VBAT pad, clamp below VDDA at 25.2 V
  (6S) max, Schottky to GND (zero negative-injection tolerance, see
  pinmap). Values fitted 2026-08-16: R1 100k (C25803,
  0603WAF1003T5E) over R2 12k (C22790, 0603WAF1202T5E), ratio
  0.10714, so 2.70 V at 25.2 V and VDDA only at 30.8 V. Same
  UNI-ROYAL 0603WAF family as the other resistors, so no new part
  class and no added feeder fee. The high-impedance choice (0.22 mA
  drain, 10.7 k source) is deliberate: C7 sits at the pin and the
  conversion runs at about 1 Hz, so the settling cost is free, and it
  keeps the pack from being drained by the sense chain.
- Decoupling set per the pinmap summary: 4x 100 nF + 4.7 uF (VDD),
  100 nF + 1 uF (VDDA), 4.7 uF plain X5R MLCC (VCAP_1 - the DS11853 ESR
  window is knowingly not honoured, see the closeout section), 100 nF
  (NRST), 100 nF (VBAT, optional).

## Cost shape

Unique Extended references on the assembled side: USB-C, ESD, crystal,
LDO, buzzer, inductor, power mux (added 2026-08-17), plus the
Standard-only sensors - roughly 7 feeder fees (~21 USD fixed) on top of
the setup/stencil/X-ray baseline of `sensor-board-components.md` (the 3
JST references and their fees left with the pads decision). Per-board
secondary parts add about $2.5 to the ~15 USD main BOM. The SWD header
is never assembled (bare through-hole footprint, MCU-2); remaining
SRC-3 trim if the quote needs it: buzzer. Volatile stock to re-check at
order time: crystal (435), inductor (4.1k), LDO (1.3k).

Delta of the 2026-08-17 change, at the 5-board Standard quote: the BOM
goes from 30 to 32 assembled lines and from 60 to 64 pieces. Parts:
+$0.45 (mux), +$0.01 (33k, 330R, third 22 uF), -$0.04 (SS54 pair gone).
Feeders: Standard charges per unique reference in every class, so the
three new lines (mux, 33k, 330R) cost about $4 of feeder while the SS54
line gives ~$1.33 back. Net, call it $4 on a ~200 EUR order - the
change is not a cost decision either way.

## Secondary parts closed out (2026-08-15)

The last seven schematic lines with an empty LCSC field are now filled.
Prices, stock and class re-read from the JLCPCB parts API on 2026-08-15
(same method as the 2026-08-12 pass).

| Ref | Role | MPN | LCSC | Package | Class | Stock | Price @1 | Notes |
|---|---|---|---|---|---|---|---|---|
| D4 | Status LED 1, red | KT-0603R | C2286 | 0603 | Basic | 6.36M | $0.0074 | ~1.0-1.7 mA through R13 = 1k at 3V3, i.e. 10-25 mcd, enough for an indoor indicator |
| D5 | Status LED 2, yellow | NCD0603Y2 | C89811 | 0603 | Preferred-Extended | 37.3k | $0.0195 | No Basic yellow 0603 worth fitting; Preferred means no feeder fee on Economic PCBA |
| R17 | Buzzer series resistor 5R1 | 0805W8F510KT5E | C17724 | 0805 | Basic | 1.04M | $0.006 | No fee-free 6.8 ohm 0805 exists; 5.1 ohm leaves the coil at 3.37 V vs its 3.6 V rating and is the fee-free option with the lowest resistor dissipation |
| C12, C16 | VDD bulk + VCAP_1, 4.7 uF 25 V X5R | CL21A475KAQNNNE | C1779 | 0805 | Basic | 3.38M | $0.0436 | One BOM line for both; C16 is the VCAP decision recorded below |
| C15 | VDDA 1 uF 50 V X5R | CL10A105KB8NNNC | C15849 | 0603 | Basic | 11.65M | $0.0056 | |
| C18, C19 | HSE load caps 8.2 pF C0G 50 V | 0603CG8R2C500NT | C1685 | 0603 | Preferred-Extended | 74.9k | $0.0093 | Exact computed value; tolerance C = +/-0.25 pF inferred from the FH part code, confirm on the datasheet at order time |
| D3 | ADC clamp Schottky | Hongjiacheng BAT54W | C7502705 | SOD-123 | Preferred-Extended | 168k | $0.0108 | IR 2 uA max at 25 V |

Rejected with numbers:

- 10 pF C0G Basic (C1634) instead of the exact 8.2 pF: the extra load
  drops the AN2867 gain margin from ~5.7 to ~4.75, under the >= 5 rule
  the crystal was selected on. Not worth the saving.
- B5819W, the only Basic Schottky, instead of the BAT54W on D3: its
  reverse leakage is 0.5-1 mA against 2 uA for the BAT54W, i.e. 250-500x
  too much for a high-impedance ADC divider node.

### Preferred-Extended parts have no feeder fee

The JLCPCB parts API exposes a `preferredComponentFlag` next to
`componentLibraryType`. Counting both: 351 Basic + 1235 Preferred = 1586
references that carry no feeder fee on Economic PCBA. The flag is
API-only; it does not appear on the rendered part pages, which is why
the earlier passes treated every non-Basic part as a fee.

Caveat that limits the value of this: on STANDARD PCBA - which the
SRC-1 LGA sensors force - JLCPCB charges roughly $1.5 per unique
reference regardless of class. The Preferred optimisation only pays if
an Economic run ever becomes viable for this board.

### Corrections to the 2026-08-12 figures

- W25Q128JVSIQ (C97521) is now BASIC, not Extended. Remove it from the
  feeder-fee count.
- STM32F722RET6 (C118207): $10.04 @1, stock 1364 - it was $4.16 on
  2026-08-12. Add the MCU to the volatile list; the price more than
  doubled in three days.
- Crystal C712812: stock 492 (was 435).
- Inductor C48496: stock 2401 (was 4.1k).
- LDO C15578: stock 6512 (was 1.3k).

### VCAP_1: plain MLCC fitted, datasheet window not honoured

C16 is a plain 4.7 uF X5R MLCC (C1779), decided 2026-08-15. DS11853
Table 19 gives an ESR *window* of 0.1-0.2 ohm for the VCAP capacitor,
i.e. a floor as well as a ceiling, which a modern MLCC (a few mohm)
misses by two orders of magnitude. That window is contradicted by ST's
own practice:

- AN4661 Rev 5 describes the same component as "ceramic", with no ESR
  floor attached.
- Every ST evaluation board fits a plain MLCC: Nucleo-64 MB1136, the
  single-VCAP layout that matches this design, carries a 4.7 uF ceramic
  specified only as "ESR < 1 ohm"; Nucleo-144 MB1137 (F722ZE, the same
  silicon as this board) carries 2.2 uF X7R.
- Newer families dropped the window entirely and spec ESR as a maximum
  only: H7 < 100 mohm, U5 < 20 mohm.

Risk statement, stated plainly: this is a knowing departure from the
letter of the F722 datasheet. The failure mode would be an under-damped
internal 1.2 V core regulator loop, showing up as rare unexplained
resets. The evidence that it does not occur is strong - ST ships the
same arrangement on its own boards and no field failure attributable to
a low-ESR VCAP cap has ever been reported - but the deviation is
recorded here rather than hidden, and it is the first thing to revisit
if the board ever shows an unexplained reset.
