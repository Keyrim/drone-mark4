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
| Load caps 2x 8.2 pF NP0 0603 | any Basic NP0 | - | - | - | Basic | C = 2 x (CL 9 - stray 5) = 8 pF -> 8.2 E12; trim 6.8/10 on bench if layout runs long |
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
| ESC connector (ESC-3) | JST SM08B-SRSS-TB | C160407 | $0.33 | 205k | Extended | Same part as the sensor-board study; mark1 pinout copied at capture |
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
  8540 coil is a 3.6 V class part on a ~4.6 V rail: either cap TIM3
  duty at ~60% in firmware or fit a 6.8 ohm series resistor - pick at
  capture.
- Connectors on board edges, openings outward (MEC-2), side entry.
- ADC divider on PC0: clamp below VDDA at 12 V max, Schottky to GND
  (zero negative-injection tolerance, see pinmap).
- Decoupling set per the pinmap summary: 4x 100 nF + 4.7 uF (VDD),
  100 nF + 1 uF (VDDA), 4.7 uF ESR 0.1-0.2 ohm (VCAP_1, the ESR window
  is a spec, not a preference), 100 nF (NRST), 100 nF (VBAT, optional).

## Cost shape

Unique Extended references on the assembled side: USB-C, ESD, crystal,
LDO, buzzer, 3x JST, inductor, plus the Standard-only sensors - roughly
9 feeder fees (~27 USD fixed) on top of the setup/stencil/X-ray
baseline of `sensor-board-components.md`. Per-board secondary parts add
about $2.5 to the ~15 USD main BOM. The SWD header is never assembled
(bare through-hole footprint, MCU-2); remaining SRC-3 trims if the
quote needs it: buzzer and spare UART connector. Volatile stock to
re-check at order time: crystal (435), inductor (4.1k), LDO (1.3k).
