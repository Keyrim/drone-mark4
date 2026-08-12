# mark4-fc rev A - peripheral resource budget

Intermediate design step between the requirements (`aio-board-spec.md`)
and the pin map (O-7): every requirement is mapped to a peripheral
INSTANCE, a transfer mode (poll / IRQ / DMA) and, where DMA is used, an
exact stream, before any pin is named. The scarce resources on an
STM32F7 are not pins but peripheral instances and DMA streams: the DMA
request matrix is fixed in silicon (each request exists only on given
stream x channel couples, RM0431 tables 26/27), so collisions must be
found here, not during layout.

DMA stream assignments below were taken from the F4/F7 family matrix
and MUST be re-verified line by line against RM0431 when the pin map is
written.

## 1. Clock domains (216 MHz sysclk, per MCU-4)

| Bus | Clock | Timer clock | Hosts used here |
|---|---|---|---|
| APB2 | 108 MHz | 216 MHz | SPI1, USART1, USART6, TIM1, ADC1 |
| APB1 | 54 MHz | 108 MHz | SPI2, I2C1, USART2, USART3, TIM2, TIM6 |

Rule applied: the speed-critical SPI (IMU) goes to APB2; everything
with relaxed timing lives on APB1.

## 2. Budget: requirement to resource

| Requirement | Instance | Mode | DMA | IRQ | Notes |
|---|---|---|---|---|---|
| Motors, DShot-ready (ESC-1) | TIM8 CH1-4 | DMA burst | DMA2 S1C7 (TIM8_UP, DMAR) | DMA2 S1 TC | Buffers in SRAM1 per MCU-6; PWM first, DShot is firmware-only. TIM1 is impossible on LQFP64 with USB: TIM1_CH4 is PA11 = USB_DM (see section 7) |
| IMU LSM6DSR (SNS-1) | SPI1 | poll first, DMA later | DMA2 S0C3 RX / S3C3 TX reserved | none (poll) | 108/16 = 6.75 MHz SCK (IMU limit 10 MHz); 14-byte burst = ~17 us, poll is fine at 500 Hz |
| IMU data-ready (SNS-2) | EXTI on INT1 | IRQ | - | EXTI line | Routed, unused while the loop stays timer-paced |
| Blackbox flash W25Q128 (STO-1) | SPI2 | DMA TX | DMA1 S4C0 TX / S3C0 RX | DMA1 S4 TC | 54/2 = 27 MHz SCK; background page writes from the RAM ring (STO-5); DMA1 is otherwise empty = zero conflict domain |
| Baro SPA06 (SNS-3) | I2C1 | poll | - | none | Slow conversion state machine, same shape as the MS5611 driver |
| Telemetry UART (CON-1) | USART1 | IRQ first, DMA TX later | DMA2 S7C4 TX reserved | USART1 | The per-byte IRQ at 921600 was measured ~3% CPU on mark1; DMA is the known upgrade |
| ESC telemetry RX (ESC-2) | USART2 RX | IRQ | - | USART2 | mark1 ESC connector carried UART2_RX; kept |
| Spare UART / RC receiver (CON-3) | USART3 | IRQ | - | USART3 | Unpopulated path until a radio exists |
| GPS (CON-2) | UART4 | IRQ | none (UART4_TX DMA would be DMA1 S4C4, colliding with the flash SPI2_TX S4C0; GPS never needs DMA) | UART4 | USART6 lost its pins to the motors (PC6/PC7, section 7); UART4 lives on PA0/PA1 |
| USB bench (CON-5) | OTG_FS | own engine | - | OTG_FS | Dedicated peripheral, PA11/PA12 fixed |
| 12 V rail sense (PWR-4) | ADC1, 1 ch | poll | - | none | One conversion around 1 Hz; no DMA, no IRQ |
| Loop pacing | TIM6 | IRQ | - | TIM6 | Basic timer, no pins; replaces mark1 TIM3 pacing (TIM3 is freed) |
| Timestamp clock | TIM2 | free-run | - | none | 32-bit at 1 MHz; mark1 used it the same way |
| Buzzer (IND-2) | GPIO | - | - | none | Transistor-driven; a spare timer channel MAY give tones later |
| LEDs x2 (IND-1) | GPIO | - | - | none | |
| SWD + SWO (MCU-2) | DAP | - | - | none | SWO is PB3, fixed: SPI1 must therefore use the PA5 SCK option, not PB3 (first pin-map constraint found here) |

## 3. DMA stream reservations

One stream runs one channel: a stream appearing twice would be a
collision. Streams not listed are free.

DMA2 (hosts every APB2 request; also QUADSPI/AES, not "APB2 only"):

| Stream | Channel | Client | Status |
|---|---|---|---|
| S0 | C3 | SPI1_RX (IMU) | reserved, optional |
| S1 | C7 | TIM8_UP (DShot) | committed |
| S3 | C3 | SPI1_TX (IMU) | reserved, optional |
| S7 | C4 | USART1_TX (telemetry) | reserved |

Collisions found and dodged by construction (verified against RM0431
Rev 3 tables 26/27, p.221):

- UART4_TX exists ONLY on DMA1 S4C4, the committed flash SPI2_TX
  stream: the GPS UART is poll/IRQ only, forever.
- USART3_TX exists ONLY on DMA1 S3C4 and S4C7 - both flash streams:
  the spare/RC UART is poll/IRQ only too (its RX side is what matters
  for a receiver anyway).
- USART2_RX (ESC telemetry) has exactly ONE stream, DMA1 S5C4 - free
  today, but zero alternatives if something ever wants S5.
- USART6_RX on S1C5 would collide with TIM8_UP S1C7, but S2C5 exists
  as an alternative - moot anyway, GPS moved to UART4 for pin reasons.

This is exactly why this document exists.

DMA1 (APB1 clients only):

| Stream | Channel | Client | Status |
|---|---|---|---|
| S3 | C0 | SPI2_RX (flash dump) | reserved |
| S4 | C0 | SPI2_TX (flash write) | committed |

## 4. Timers

| Timer | Use | Notes |
|---|---|---|
| TIM8 | Motors CH1-4 | Advanced timer, DMA-capable, APB2; PC6-PC9 all exist on LQFP64 |
| TIM2 | 1 MHz 32-bit free-run timestamp | never overflows in a flight |
| TIM6 | Loop pacer IRQ (500 Hz to 1 kHz) | basic timer, no pins |
| TIM1/3/4/5/7... | free | TIM1 unusable for 4 motor channels with USB (PA11); TIM3 no longer carries motors (mark1 legacy removed) |

## 5. Interrupt priority ordering (relative, numbers at firmware time)

1. TIM6 pacer (the loop heartbeat; everything else yields to it)
2. DMA2 S1 (DShot frame complete; short, timing-sensitive)
3. USART1 / USART2 / USART3 (byte rings, must not overrun)
4. DMA1 S4 (flash write complete; latency-tolerant by design, the RAM
   ring absorbs it)
5. OTG_FS (bench only, lowest)

EXTI (IMU INT1) is unranked until data-ready pacing is actually
adopted; it would then take the pacer's slot.

## 6. Headroom (unused instances)

SPI3, I2C2, I2C3, UART5, USART6 (instance free, its pins are not),
TIM1/3/4/5/7/9-14, ADC2/3, plenty of free DMA streams on both
controllers. No requirement consumes the last instance of any
peripheral class.

## 7. Feeds into the pin map (O-7)

The one hard conflict, found and resolved here: on the LQFP64 package
TIM1 CH1-4 land on PA8/PA9/PA10/PA11 (the PE alternates do not exist),
and PA11/PA12 belong to USB (CON-5). Four motor channels on TIM1 and
USB are mutually exclusive on this package. Resolution: motors on TIM8
CH1-4 = PC6/PC7/PC8/PC9 (AF3), the same shape commercial F722 FCs use.
Knock-on: PC6/PC7 were the only USART6 pins, so the GPS UART moves to
UART4 (PA0/PA1).

Constraints already fixed by this budget:

- Motors: PC6/PC7/PC8/PC9 (TIM8 AF3).
- SPI1 on the PA5/PA6/PA7 group (PB3 is SWO).
- PA11/PA12 for OTG_FS; PA3 for USART2_RX (mark1 ESC pinout reuse);
  USART1 on PB6/PB7 (PA9/PA10 stay free for other uses).
- GPS UART4 on PA0/PA1.
- Everything else (I2C1, SPI2, USART3, ADC channel, EXTI, GPIOs) has
  multiple candidate groups and follows.

Verification checklist for the pin map: every DMA line above against
RM0431, TIM8 AF3 pins on the 64-pin package, 5 V tolerance where a pin
meets the outside world.
