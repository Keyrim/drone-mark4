# mark4-fc rev A - STM32F722RET6 pin map

Full pin map for the LQFP64 STM32F722RET6, closing O-7 of
`aio-board-spec.md`. It sits on top of the peripheral and DMA budget of
`aio-board-design.md`: every instance, transfer mode and DMA stream fixed
there is taken as given here, and only the pins that document left open
are decided below.

Ground truth for pin positions, alternate function numbers and DMA
request mappings: the machine-readable STM32F722RETx description
extracted from ST's CubeMX database (`STM32F722RE.json` of the
stm32-data project), cross-checked against RM0431 Rev 3 where the JSON
carries no data (debug port pins). The `Verify` column says:

- `json` - position, signal and AF number read back from the package and
  peripheral tables of that JSON.
- `json+rm` - position from the JSON, function from RM0431 (the JSON has
  no DBGMCU pin table).
- `n/a` - plain GPIO or a supply/reset/boot pin: nothing to check beyond
  the position, which is from the JSON package list.

Verification status: a second, independent pass checked all 64
positions and every AF against DS11853 Rev 9 (Figure 15 p.48, Table 10
p.60-78, Table 12 p.84-89) and every DMA line against RM0431 Rev 3
tables 26/27 (p.221). All positions and AFs confirmed; the findings of
that pass (5 V tolerance, VCAP ESR, VBUS sensing, strapping) are folded
into the table and the resolved open items below.

Package facts worth knowing before reading the table (JSON package
list, confirmed on DS11853):

- LQFP64 on this die exposes PC0..PC4 and PC6..PC15, but **not PC5**.
  ADC1_IN15 (PC5) therefore does not exist on this board.
- Only one core-supply capacitor pin, `VCAP_1` (position 30); there is no
  VCAP_2 on this package.
- No separate VREF+ pin: the ADC reference is VDDA. Known DS11853 Rev 9
  internal contradiction: Table 10 (p.63) puts VREF+ on position 13 for
  LQFP64, but Figure 15 (p.48) and Figure 50 note 1 (p.155) say VDDA -
  VDDA is correct, do not "fix" pin 13 the other way.
- 5 V tolerance (DS11853 Table 9 legend + Table 10): every signal pin
  used here is FT, and PB6/PB7/PB8/PB9/PB10/PB11 are FTf (I2C Fm+
  capable). The only non-FT pins in use: PA4 and PA5 are TTa (3.3 V
  max, the only two such GPIOs on the package - internal IMU CS/SCK
  nets, nothing 5 V may ever touch them), NRST is RST type (4.0 V max,
  exposed on the SWD header), BOOT0 is B type (9.0 V max).
- No PE/PF/PG port pins at all, and from GPIOD only PD2.
- HSE lands on PH0/PH1 (positions 5 and 6), which are OSC_IN/OSC_OUT
  only pads in practice.

## 1. Pin table (all 64 positions)

| # | Pin | Function | AF | Req | Verify | Note |
|---|---|---|---|---|---|---|
| 1 | VBAT | Power: tie to 3V3 | - | - | n/a | No coin cell, no RTC backup wanted; short to VDD, 100 nF to GND |
| 2 | PC13 | free | - | - | n/a | Test point only. Backup-domain pin, weak drive, avoid for LEDs |
| 3 | PC14 | free (OSC32_IN) | - | - | json | No LSE crystal fitted; usable as slow GPIO / test point |
| 4 | PC15 | free (OSC32_OUT) | - | - | json | Same as PC14 |
| 5 | PH0 | HSE OSC_IN | - | MCU-4 | json | 8 MHz crystal, two load caps per crystal datasheet, guard ring |
| 6 | PH1 | HSE OSC_OUT | - | MCU-4 | json | Crystal other terminal; keep the loop tiny, no traces underneath |
| 7 | NRST | Reset | - | MCU-2 | n/a | 100 nF to GND at the pin (DS Figure 47); internal pull-up 30-50k. NOT 5 V tolerant (4.0 V max). No longer brought out since the header went to 3 positions (2026-08-18): no connect-under-reset, BOOT0 is the recovery path |
| 8 | PC0 | ADC1_IN10, VBAT sense | analog | PWR-4 | json | Divider from the VBAT pad (25.2 V max, 6S), ~100 nF to GND at the pin, plus Schottky to GND (DS Table 60: zero negative-injection tolerance on PC0..PC3) |
| 9 | PC1 | ADC1_IN11, ESC current-sense pad | analog | PWR-4 | json | Pad only, may be DNP (SRC-3) |
| 10 | PC2 | free | - | - | n/a | Analog-capable (ADC1_IN12) test point |
| 11 | PC3 | free | - | - | n/a | Analog-capable (ADC1_IN13) test point |
| 12 | VSSA | Analog ground | - | - | n/a | Joined to the ground plane at one point near the MCU |
| 13 | VDDA | Analog supply | - | - | n/a | 100 nF + 1 uF (DS Figure 28); optional ferrite from 3V3, NO series resistor (VDDA is the ADC reference, IR drop = reference offset). See the Table 10 erratum note above |
| 14 | PA0 | UART4_TX (GPS) | AF8 | CON-2 | json | |
| 15 | PA1 | UART4_RX (GPS) | AF8 | CON-2 | json | |
| 16 | PA2 | USART2_TX pad (ESC connector) | AF7 | ESC-2, ESC-3 | json | Optional: only RX is required; pad kept so the ESC link can talk back |
| 17 | PA3 | USART2_RX (ESC telemetry) | AF7 | ESC-2 | json | mark1 ESC connector pinout reuse |
| 18 | VSS | Ground | - | - | n/a | |
| 19 | VDD | 3V3 | - | PWR-2 | n/a | 100 nF at the pin |
| 20 | PA4 | GPIO out, IMU chip select | - | SNS-1 | n/a | Software CS (SPI1_NSS also exists here, AF5, unused); dedicated CS per SNS-1. TTa pin: 3.3 V max |
| 21 | PA5 | SPI1_SCK (IMU) | AF5 | SNS-1 | json | PA5 group forced by PB3 = SWO. TTa pin: 3.3 V max |
| 22 | PA6 | SPI1_MISO (IMU) | AF5 | SNS-1 | json | |
| 23 | PA7 | SPI1_MOSI (IMU) | AF5 | SNS-1 | json | |
| 24 | PC4 | GPIO in, IMU INT1, EXTI line 4 | - | SNS-2 | n/a | Adjacent to the SPI1 group; EXTI4 otherwise unused. NO pull-up on this net ever (INT1 is an IMU interface strap at power-up); optional 10k pull-down |
| 25 | PB0 | GPIO out, LED1 | - | IND-1 | n/a | TIM3_CH3 (AF2) available later for dimming |
| 26 | PB1 | GPIO out, LED2 | - | IND-1 | n/a | TIM3_CH4 (AF2) available later for dimming |
| 27 | PB2 | free | - | - | n/a | Test point; the JSON lists no BOOT1 signal on this die |
| 28 | PB10 | USART3_TX (spare UART) | AF7 | CON-3 | json | |
| 29 | PB11 | USART3_RX (spare UART) | AF7 | CON-3 | json | RC receiver path, may be DNP (SRC-3) |
| 30 | VCAP_1 | Core regulator cap | - | - | json | 4.7 uF to VSS, ESR in the 0.1-0.2 Ohm WINDOW (DS Table 19 - a too-good MLCC is out of spec, floor included), short and close. Only VCAP pin on this package (confirmed, VCAP_2 absent) |
| 31 | VSS | Ground | - | - | n/a | |
| 32 | VDD | 3V3 | - | PWR-2 | n/a | 100 nF at the pin |
| 33 | PB12 | GPIO out, flash chip select | - | STO-1 | n/a | Software CS (SPI2_NSS also here, AF5, unused). 10k pull-up to 3V3: the W25Q /CS must track VCC at power-up while this pin is still high-Z |
| 34 | PB13 | SPI2_SCK (flash) | AF5 | STO-1 | json | |
| 35 | PB14 | SPI2_MISO (flash) | AF5 | STO-1 | json | |
| 36 | PB15 | SPI2_MOSI (flash) | AF5 | STO-1 | json | |
| 37 | PC6 | TIM8_CH1, motor 1 | AF3 | ESC-1 | json | |
| 38 | PC7 | TIM8_CH2, motor 2 | AF3 | ESC-1 | json | |
| 39 | PC8 | TIM8_CH3, motor 3 | AF3 | ESC-1 | json | |
| 40 | PC9 | TIM8_CH4, motor 4 | AF3 | ESC-1 | json | |
| 41 | PA8 | free | - | - | n/a | Good test point: MCO_1 (AF0) for clock bring-up |
| 42 | PA9 | free | - | - | json | OTG_FS_VBUS is an ADDITIONAL function here (no AF involved). VBUS sensing stays disabled (GCCFG.VBDEN=0 is the reset state) and firmware forces B-valid via GOTGCTL BVALOEN=1 + BVALOVAL=1; no divider, pin fully free. If ever reclaimed with sensing on, mind the internal 2.4-8k pull-down |
| 43 | PA10 | TIM1_CH3, addressable LED strip data | AF1 | IND-3 | json | Assigned 2026-08-17. PWM + DMA2 S6C6 for WS2812 timing; 330 R in series to J8. OTG_FS_ID (AF10) is what is given up, and it is useless on a device-only board (this is why PA10 was taken and not PA9, which keeps VBUS sensing as an option) |
| 44 | PA11 | OTG_FS_DM | AF10 | CON-5 | json | 90 R differential pair with PA12, ESD array, no series resistors |
| 45 | PA12 | OTG_FS_DP | AF10 | CON-5 | json | |
| 46 | PA13 | SWDIO, SWD header pin 1 | AF0 | MCU-2, CON-4 | json+rm | Internal pull-up at reset; keep the trace short |
| 47 | VSS | Ground | - | - | n/a | |
| 48 | VDD | 3V3 | - | PWR-2 | n/a | 100 nF at the pin |
| 49 | PA14 | SWCLK, SWD header pin 2 | AF0 | MCU-2, CON-4 | json+rm | Internal pull-down at reset |
| 50 | PA15 | free | - | - | n/a | JTDI at reset (AF0); usable as GPIO once MODER is set. Test point |
| 51 | PC10 | free | - | - | n/a | Alternate USART3_TX / UART4_TX if routing forces a change |
| 52 | PC11 | free | - | - | n/a | Alternate USART3_RX / UART4_RX |
| 53 | PC12 | GPIO in, power source status | - | PWR-3 | n/a | Assigned 2026-08-17. Open-drain ST output of the U8 power mux with a 10k pull-up to 3V3: high = running on the ESC BEC, low = running on bench USB or thermal shutdown. Plain input, no AF |
| 54 | PD2 | free | - | - | n/a | Only GPIOD pin on this package. Test point |
| 55 | PB3 | free (JTDO/TRACESWO) | AF0 | - | json+rm | Was the SWO header pin until 2026-08-18; unconnected now. It is still why SPI1 uses PA5 rather than PB3, and it is the pin to take if trace output is ever wanted |
| 56 | PB4 | free | - | - | n/a | NJTRST at reset (AF0); usable as GPIO. Test point |
| 57 | PB5 | GPIO out, buzzer drive | - | IND-2 | n/a | Transistor-driven; TIM3_CH2 (AF2) on this pin gives tones later |
| 58 | PB6 | USART1_TX (telemetry) | AF7 | CON-1 | json | 921600 baud bench link / ESP32 bridge |
| 59 | PB7 | USART1_RX (telemetry) | AF7 | CON-1 | json | |
| 60 | BOOT0 | Boot select | - | MCU-3 | n/a | 10 k pull-down to GND plus a jumper/solder bridge to 3V3 for DFU. Input-only B-type pin (9 V max), no internal pull, latched on the 4th SYSCLK edge after reset; firmware must never reprogram the BOOT_ADD1 option byte or the recovery path dies |
| 61 | PB8 | I2C1_SCL (baro + GPS mag) | AF4 | SNS-3, CON-2 | json | 4.7 k pull-up to 3V3 |
| 62 | PB9 | I2C1_SDA (baro + GPS mag) | AF4 | SNS-3, CON-2 | json | 4.7 k pull-up to 3V3 |
| 63 | VSS | Ground | - | - | n/a | |
| 64 | VDD | 3V3 | - | PWR-2 | n/a | 100 nF at the pin |

No position appears twice and none is missing: the table has exactly the
64 entries of the JSON package list, in position order (swept
programmatically, see section 6).

Global decoupling summary (all four VDD pins, VDDA, VBAT, VCAP_1
covered above): 4x 100 nF one per VDD pin, 1x 4.7 uF bulk on 3V3 near
the MCU, VDDA filtered from 3V3, VCAP_1 4.7 uF, NRST 100 nF, BOOT0
pulled low with a forcing jumper.

## 2. Choices made here (one line of justification each)

Constrained pins came from `aio-board-design.md` and were only
positioned; the following were open and are decided now. Every AF was
read from the JSON.

- **I2C1 = PB8 SCL / PB9 SDA, AF4.** PB6/PB7 (the other I2C1 option) are
  taken by USART1, and PB8/PB9 are the only remaining I2C1 pins on this
  package (JSON lists exactly PB5 SMBA, PB6/PB7, PB8/PB9). Confirmed
  AF4.
- **SPI2 = PB13 SCK / PB14 MISO / PB15 MOSI, AF5.** SPI2_SCK exists
  only on PA9, PB10 and PB13 on this package (DS11853 Table 12); PB10
  is wanted for USART3 and PA9 sits in the USB area, so PB13..PB15 is
  the only clean contiguous group, straight run to the SOIC-8 flash.
- **Flash CS = PB12 as plain GPIO.** Next to the SPI2 group (PB12 also
  carries SPI2_NSS AF5, unused): software CS keeps the driver trivial
  and lets a single transfer span several bytes.
- **IMU CS = PA4 as plain GPIO.** Adjacent to the SPI1 group, same
  reasoning; SNS-1 only demands a dedicated CS, not a hardware NSS.
- **USART3 = PB10 TX / PB11 RX, AF7.** Free after SPI2 moved to
  PB13..PB15, contiguous, and it keeps the PC10/PC11 pair as a spare
  escape route for either USART3 or UART4 at layout time.
- **IMU INT1 = PC4, EXTI line 4.** Sits right next to PA7, so INT1 stays
  in the IMU pin cluster; EXTI4 has no other user on this board, and
  PA4 (the other EXTI4 candidate in the cluster) is the IMU CS with no
  EXTI need.
- **VBAT sense = PC0 = ADC1_IN10.** In the analog corner next to
  VSSA/VDDA (positions 12/13) and far from PA0/PA1, which UART4 owns;
  ADC1_IN0..IN7 all sit on PA0..PA7, which are taken by GPS, ESC UART
  and SPI1.
- **ESC current-sense pad = PC1 = ADC1_IN11.** Adjacent to the rail
  sense so both dividers share one analog area; pad only per PWR-4 and
  DNP-able per SRC-3.
- **LED1 = PB0, LED2 = PB1, buzzer = PB5.** All three are plain GPIOs at
  reset and all three happen to carry a TIM3 channel (CH3, CH4, CH2, all
  AF2), so brightness and tones are a firmware change on a timer
  `aio-board-design.md` lists as free; PC13/PC14/PC15 were rejected for
  LEDs (backup-domain pins, weak drive).
- **HSE = PH0 / PH1, positions 5 and 6.** JSON RCC table gives PH0 =
  OSC_IN and PH1 = OSC_OUT; they are the only HSE pins and are not
  usable as general GPIO in practice on this package.
- **LSE not fitted:** PC14/PC15 stay free. No requirement asks for an
  RTC, and VBAT is tied to 3V3.

## 3. DMA cross-check against the JSON

Every line committed or reserved in section 3 of `aio-board-design.md`,
against the JSON `dma_channels` tables. "CHn" in the JSON means stream
n; `request` is the channel selector 0..7.

| Design-doc line | JSON says | Verdict |
|---|---|---|
| TIM8_UP = DMA2 S1 C7 | TIM8 UP: DMA2_CH1, request 7 | MATCH |
| SPI1_RX = DMA2 S0 C3 | SPI1 RX: DMA2_CH0 req 3, also DMA2_CH2 req 3 | MATCH |
| SPI1_TX = DMA2 S3 C3 | SPI1 TX: DMA2_CH3 req 3, also DMA2_CH5 req 3 | MATCH |
| USART1_TX = DMA2 S7 C4 | USART1 TX: DMA2_CH7, request 4 (only entry) | MATCH |
| SPI2_TX = DMA1 S4 C0 | SPI2 TX: DMA1_CH4, request 0 (only entry) | MATCH |
| SPI2_RX = DMA1 S3 C0 | SPI2 RX: DMA1_CH3, request 0 (only entry) | MATCH |
| UART4_TX exists only on DMA1 S4 C4 | UART4 TX: DMA1_CH4, request 4 (only entry) | MATCH |
| TIM1_CH3 = DMA2 S6 C6 | TIM1 CH3: DMA2_CH6, request 6 (also request 0 on the same stream, the burst/DMAR entry) | MATCH |

So the collision the design doc used to justify a poll/IRQ-only GPS is
real: UART4_TX has exactly one stream, DMA1 S4, which is the committed
flash SPI2_TX stream.

Extra facts found while checking, none of them a problem today:

- **USART3_TX has no collision-free DMA stream either.** JSON gives
  DMA1_CH3 req 4 and DMA1_CH4 req 7, i.e. the same two streams as
  SPI2_RX (S3) and SPI2_TX (S4). The spare UART is therefore IRQ-only
  for the same reason as the GPS, as long as the flash keeps both S3 and
  S4. Now carried in the design doc's collision list.
- **USART1_RX is free**: DMA2_CH2 req 4 or DMA2_CH5 req 4. Both streams
  are also SPI1 alternates (SPI1_RX on S2, SPI1_TX on S5), so if
  USART1_RX ever goes DMA, SPI1 must stay on its primary S0/S3 pair -
  which is what the design doc reserved.
- **TIM8 per-channel DMA** (CH1..CH4) exists on DMA2 S2/S3/S4/S7. Not
  needed: DShot uses TIM8_UP with DMAR burst, which is the reserved S1
  line. Note S7 (TIM8_CH4/TRIG/COM) is the same stream as the reserved
  USART1_TX, so per-channel TIM8 DMA is not an available fallback
  without moving telemetry.
- **ADC1 DMA** would be DMA2 S0 req 0 or S4 req 0; S0 is the SPI1_RX
  reservation. Irrelevant at ~1 Hz polling (PWR-4).

## 4. Free pins (test point candidates)

13 GPIOs are unassigned:

| Pin | # | Why it is interesting |
|---|---|---|
| PC13 | 2 | Backup-domain pin, weak drive: input/test point only |
| PC14 | 3 | OSC32_IN, no LSE fitted |
| PC15 | 4 | OSC32_OUT, no LSE fitted |
| PC2 | 10 | ADC1_IN12, spare analog input |
| PC3 | 11 | ADC1_IN13, spare analog input |
| PB2 | 27 | Plain GPIO |
| PA8 | 41 | MCO_1 (AF0): clock bring-up probe; also TIM1_CH1 |
| PA9 | 42 | OTG_FS VBUS sense if USB VBUS detection is wanted; also TIM1_CH2 |
| PA15 | 50 | JTDI at reset; TIM2_CH1 |
| PC10 | 51 | USART3_TX or UART4_TX alternate |
| PC11 | 52 | USART3_RX or UART4_RX alternate |
| PD2 | 54 | Only GPIOD pin; UART5_RX alternate |
| PB4 | 56 | NJTRST at reset; TIM3_CH1 |

Plus PA2 (16), which is populated as a pad only (USART2_TX towards the
ESC connector) and can be reclaimed if the ESC never needs a TX line.

Recommendation: bring PA8, PB2, PB4 and PD2 out as actual pads or
0.1 inch-spaced vias; the rest are reachable at the package if needed.

## 5. Conflicts

None. Every constraint fixed in `aio-board-design.md` (motors on
PC6..PC9 AF3, SPI1 on PA5/PA6/PA7, USART1 on PB6/PB7, USART2_RX on PA3,
UART4 on PA0/PA1, OTG_FS on PA11/PA12, SWD on PA13/PA14 plus SWO on
PB3) is honoured verbatim and verified in the JSON; no row is marked
CONFLICT. The one thing the design doc got exactly right by construction
is worth restating: with USB present, TIM1 cannot carry four motor
channels on this package, and TIM8 CH1..CH4 do all exist on LQFP64
(positions 37..40).

## 6. Method note

The table was produced by dumping `.packages[0].pins` (64 entries,
sorted by position) and each relevant peripheral's `.pins[]` and
`.dma_channels[]` from `STM32F722RE.json` with short python one-liners,
then filling assignments into that dump so that a missing or
double-assigned position would be visible immediately. RM0431 Rev 3 was
used only for the debug port pins (JTMS/SWDIO = PA13, JTCK/SWCLK = PA14,
JTDO/TRACESWO = PB3) and for the PC13/PC14/PC15 backup-domain caveat,
since the JSON has no DBGMCU pin table.

## 7. Open items

Resolved 2026-08-12 by the DS11853/RM0431 verification pass:

- 5 V tolerance: CLOSED, see the package-facts list (everything used
  is FT/FTf; PA4/PA5 TTa internal-only; NRST not FT; the PC0 divider
  must clamp below VDDA at the VBAT max, 25.2 V / 6S, with the
  Schottky of the table note).
- VCAP_2: CLOSED - absent on LQFP64 (DS 6.3.2 note, Table 19), one
  4.7 uF ESR 0.1-0.2 Ohm on position 30.
- PB2/BOOT1: CLOSED - PB2 is a plain FT GPIO on F72x, no boot
  function, no AF at all; free pin confirmed.
- USB VBUS sensing: CLOSED - stays disabled, firmware forces B-valid
  (GOTGCTL BVALOEN/BVALOVAL), PA9 fully free, no divider footprint
  needed.
- USART3_TX DMA collision: applied to the design doc's collision list.

Still open:

1. **TIM8 complementary outputs are consumed.** PB14/PB15 carry
   TIM8_CH2N/CH3N and are used by SPI2 here. Harmless for PWM and for
   DShot (single-ended outputs), but complementary motor drive would be
   impossible without moving the flash bus. Recording it so nobody
   rediscovers it during a respin.
2. **Bidirectional DShot (ESC telemetry over the motor lines)** is not
   pin-blocked but is DMA-tight: input capture on TIM8 channels would
   want DMA2 S2/S3/S4/S7, and S3 and S7 are reserved for SPI1_TX and
   USART1_TX. If bidirectional DShot ever becomes a goal, the stream
   plan needs a second pass; the ESC telemetry UART on PA3 (ESC-2) is
   the reason this is not urgent.
3. **Crystal choice** for the 8 MHz HSE (SRC-1). There is no HSE
   drive-level register on this family; the datasheet constraints are
   Gm_crit_max = 1 mA/V (bounds the crystal ESR choice) and load caps
   in the 5-25 pF range minus roughly 10 pF of pin+board stray
   (DS11853 Table 40 and notes, AN2867 for the margin math). The
   +/-500 ppm HSE accuracy bound is what makes the crystal mandatory
   for USB (MCU-4).
4. **Firmware bring-up checklist items recorded here so they are not
   lost** (no board impact): 216 MHz needs voltage scale 1 + over-drive
   and 7 flash wait states at 3.3 V; APB1/APB2 at 54/108 MHz assume
   over-drive on; IMU first writes I2C_disable/I3C_disable (SNS-1);
   USB B-valid override (PA9 row); never touch BOOT_ADD1 (BOOT0 row).
