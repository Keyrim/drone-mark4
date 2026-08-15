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
| Blackbox flash | W25Q128JVSIQ | C97521 | ~$1.70 | STO-1 strapping: /CS, /WP, /HOLD 10k pull-ups |
| 5 V buck | AP63205WU-7 | C2071056 | ~$0.40 | PWR-1; L/C per its Table 3, EN to VIN, BST 100 nF to SW |

## USB and power OR

| Role | MPN | LCSC | Price | Stock | Class | Notes |
|---|---|---|---|---|---|---|
| USB-C receptacle 16p USB2.0 | HRO TYPE-C-31-M-12 | C165948 | $0.19 | 446k | Extended | De facto JLC standard; no 16-pin USB-C exists in Basic at all |
| ESD array D+/D-/VBUS | ST USBLC6-2SC6 | C7519 | $0.18 | 31k | Extended | Flow-through pinout; all USBLC6 types are Extended, no saving in the clone |
| OR Schottky x2 | MDD SS54 (5 A/40 V, SMA) | C22452 | $0.04 | 2.8M | Basic | 5 A die runs low on its Vf curve: USB leg worst case 4.75 - 0.33 = 4.42 V (above the 4.4 V floor), buck leg 5.0 - 0.35 at 1 A = 4.65 V |
| CC pulldowns 2x 5.1k 0603 | UNI-ROYAL 0603WAF5101T5E | C23186 | $0.004 | 9M | Basic | One per CC pin, never shared; nothing else needed for a UFP-only port |

Alternates: TYPE-C-31-M-31 (C2760486, verify footprint before swapping);
SS34 (C8678, Basic, drop-in, slightly worse Vf). Ideal-diode ICs
rejected: no true 2-input OR part under $0.50 at LCSC (LM66100 is
single-input, needs two + an Extended slot).

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
| UART connectors x2 (CON-1, CON-3) | JST SM04B-SRSS-TB | C160404 | $0.43 | 11k | Extended | Genuine JST, SIDE entry (SM..B; the BM..B variants are top-entry, do not substitute) |
| GPS connector (CON-2) | JST SM06B-SRSS-TB | C160405 | $0.30 | 15k | Extended | 3V3/GND/UART/I2C = 6 pins exactly |
| ESC connector (ESC-3) | JST SM08B-SRSS-TB | C160407 | $0.33 | 205k | Extended | Same part as the sensor-board study; pinout frozen in `aio-board-spec.md` ESC-3 |
| SWD header (CON-4) | through-hole 2.54 mm 1x6 footprint | none | - | - | not assembled | No BOM line: hand-populated with 3 pins from bench stock (GND/SWCLK/SWDIO per MCU-2); all 6 positions routed |
| Buck inductor 4.7 uH | Sunlord SWPA5040S4R7MT | C48496 | $0.08 | 4.1k (THIN) | Extended | Shielded 5x5x4; Isat 3.5 A min (>= 2.7 required), DCR 30 mOhm |
| Buck CIN 10 uF/25 V 0805 | Samsung CL21A106KAYNNNE | C15850 | $0.10 | 10M | Basic | |
| Buck COUT 2x 22 uF/25 V 0805 | Samsung CL21A226MAQNNNE | C45783 | $0.45 | 4M | Basic | |
| BST cap 100 nF 0603 | YAGEO CC0603KRX7R9BB104 | C14663 | $0.02 | 100M | Basic | SW-to-BST, not to GND |

Alternates: buzzer MLT-8530 (C94599, 1 mm lower, 7x stock, 80 dB min);
inductor SWPA4030S4R7MT (C57269, 4x4x3, Isat 2.9 A, 10x stock).

## Consolidated schematic notes

- OR topology: both SS54 anodes at their source (buck out, VBUS),
  cathodes joined on the shared 5 V rail; the buck-leg diode is
  mandatory (PWR-3, body-diode back-feed).
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
- Connectors on board edges, openings outward (MEC-2), side entry.
- ADC divider on PC0: clamp below VDDA at 12 V max, Schottky to GND
  (zero negative-injection tolerance, see pinmap).
- Decoupling set per the pinmap summary: 4x 100 nF + 4.7 uF (VDD),
  100 nF + 1 uF (VDDA), 4.7 uF plain X5R MLCC (VCAP_1 - the DS11853 ESR
  window is knowingly not honoured, see the closeout section), 100 nF
  (NRST), 100 nF (VBAT, optional).

## Cost shape

Unique Extended references on the assembled side: USB-C, ESD, crystal,
LDO, buzzer, 3x JST, inductor, plus the Standard-only sensors - roughly
9 feeder fees (~27 USD fixed) on top of the setup/stencil/X-ray
baseline of `sensor-board-components.md`. Per-board secondary parts add
about $2.5 to the ~15 USD main BOM. The SWD header is never assembled
(bare through-hole footprint, MCU-2); remaining SRC-3 trims if the
quote needs it: buzzer and spare UART connector. Volatile stock to
re-check at order time: crystal (435), inductor (4.1k), LDO (1.3k).

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
- JST SM04B-SRSS-TB C160404: stock 8707 (was 11k).

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
