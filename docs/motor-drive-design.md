# Motor drive - design

> Design for the first motor output on the mark1 bench: replace the null
> motor sink with a DShot600 sink driving the Velox 4-in-1 ESC. Written
> before implementation; the section 8 extensions are named, not designed.

## 1. Goal and scope

Spin the four motors of the bench drone through the existing flight
chain, end to end: the transmitter widget of the pages streams the RC
state, the flight core steps and mixes, and the `ActuatorFrame` reaches
a motor sink that actually drives the ESC instead of recording the
commands. Everything upstream already exists (kill switch first in
`step()`, FAULT cuts the motors, 500 ms RC silence trips the fail-safe,
the OTA trial interlock refuses arming); the missing piece is one
platform service implementation and one timer relocation.

In scope:

- move the flight loop pacer off TIM3 (its channels are the motor pins),
- `MotorSinkDshot` in the stm32 platform variant: DShot600 on TIM3
  CH1-4 through one burst-DMA stream,
- the bench validation procedure.

Out of scope, named as next steps in section 8: bidirectional DShot
(eRPM), the ESC telemetry UART, the current sense ADC.

## 2. Hardware facts

The ESC is a T-Motor Velox 45A V2 4-in-1 (BLHeli_32), wired to the
mark1 board; propellers removed for the whole bench phase. Power path
on the bench: battery -> the ESC directly, and battery -> an off-board
buck -> the mark1 5V rail and LDO. The signal wiring:

| Signal | MCU pin | Function (AF) | ESC side |
|---|---|---|---|
| Motor 1 | PA6 | TIM3_CH1 (AF2) | M1, rear right |
| Motor 2 | PA7 | TIM3_CH2 (AF2) | M2, front right |
| Motor 3 | PB0 | TIM3_CH3 (AF2) | M3, rear left |
| Motor 4 | PB1 | TIM3_CH4 (AF2) | M4, front left |
| ESC telemetry | PA3 | USART2_RX | TLM (shared wire), section 8 |
| Current sense | PC2 | ADC1_IN14 | analog current output, direct, section 8 |

The motor order is a gift: the mixer (`flight_core/mixer.hpp`) and the
Godot plant already number the motors like a 4-in-1 ESC (0 rear right,
1 front right, 2 rear left, 3 front left), so `ActuatorFrame::motor[i]`
maps to TIM3 channel `i+1` and ESC output `M(i+1)` with no remap
anywhere. Motor spin direction is ESC configuration (BLHeli suite), not
firmware; it is checked once on the bench.

## 3. The pacer moves to TIM6

TIM3 paces the flight loop today (`sensor_source_stm32.cpp`: 1 MHz
prescaler, update interrupt, WFI in `waitFrame()`). The mark1 routing
puts the four motor channels on that same timer, so the pacer moves to
TIM6: a basic timer with no pins, on the same APB1 clock, taking the
exact same prescaler and ARR values. Its interrupt line (position 54)
is shared with the DAC, which this board never uses. This is also the
timer the next board's design already assigns to the pacer, so nothing
diverges later.

The change is confined to `sensor_source_stm32.cpp` (register block,
RCC enable bit, IRQ number, `TIM6_DAC_IRQHandler` vector) plus the TIM6
address in `registers.hpp`.

## 4. Why DShot600

- Digital and self-clocked: no ESC throttle calibration, ever.
- Each frame carries a CRC; a noisy or floating line yields no output
  at all instead of a wrong throttle.
- The value space has an explicit motor-stop: 0 stops the motor, 1-47
  are ESC commands (unused here), 48-2047 is the throttle range. There
  is no pulse width to mistrust around zero.
- The Velox (BLHeli_32) speaks it natively, and DShot600 leaves room
  before the timing gets tight on an 84 MHz timer clock.

One frame is 16 bits, MSB first: 11 bits of value, 1 telemetry-request
bit, 4 bits of CRC (xor of the three value nibbles). Bit encoding is
pulse-width over a fixed bit period; at DShot600 on the 84 MHz APB1
timer clock:

| Quantity | Time | Timer ticks |
|---|---|---|
| Bit period | 1667 ns | 140 (ARR = 139) |
| T0H (a 0 bit) | 625 ns | 52 |
| T1H (a 1 bit) | 1250 ns | 105 |
| Whole frame | 26.7 us | - |

The line idles low between frames. One frame per motor per flight loop
(500 Hz) is 26.7 us of activity every 2 ms: far above the minimum
inter-frame gap, far below any bandwidth concern.

BLHeli_32 arms after a steady stream of valid zero-throttle frames
(about a second). The core outputs zeros whenever killed or idle, and
the sink transmits every loop from init on, so ESC arming needs no
dedicated sequence: power the bench, the arm tone follows.

## 5. MotorSinkDshot

One new implementation of `AbsMotorSink` in
`software/components/platform/src/stm32/`, replacing `MotorSinkNull`
(deleted; the DShot sink keeps the `last()` accessor the status log
line reads).

Mechanics, the standard burst-DMA pattern:

- TIM3 in PWM mode 1 on CH1-4, the four pins in AF2. CCRn holds each
  bit's high time; CCRn = 0 keeps the line low.
- The timer update event raises one DMA request (TIM3_UP -> DMA1
  stream 2, channel 5) in memory-to-peripheral burst mode through
  TIM3's DMAR/DCR window: one request writes all four CCRs. One buffer
  slot per bit per channel.
- The frame buffer is (16 bits + 2 trailing zero slots) x 4 channels =
  72 halfwords. The two trailing zero slots park all four lines low
  after the last bit.
- `push()` encodes the four motor values into the buffer and re-arms
  the stream. At 500 Hz a 27 us transfer can never still be running,
  but `push()` checks the stream is idle anyway and counts a skipped
  frame otherwise, reported like the sensor source reports overruns.
- The buffer lives in a plain member (static storage of the App): the
  default RAM region is SRAM1/2 at 0x20000000, which is DMA-capable.
  The linker script already warns that CCM is not; nothing may ever
  move this buffer to CCM.

Value mapping, stateless: `motor <= 0` sends 0 (stop);
otherwise `48 + round(motor * 1999)` clamped to 2047. The telemetry bit
stays 0 in v1. No idle floor in the sink: the sink is a transducer, a
minimum spinning throttle is flight policy and belongs to the core's
tuning the day hover work needs it.

Frame encoding (value -> 16-bit frame -> per-bit CCR values) is a pure
function in the sink's header, so a desktop Catch2 test pins it against
known DShot vectors without any hardware.

DShot silence is itself safe: when the firmware parks the loop (OTA
session) or stops, the ESC sees no frames and cuts the motors by its
own signal-loss failsafe, consistent with the update path refusing to
start while armed.

## 6. Composition changes

- `sensor_source_stm32.cpp`: TIM3 -> TIM6 (section 3).
- `registers.hpp`: TIM6 base; DMA register blocks exist already.
- `platform_stm32/motor_sink_dshot.hpp` + `motor_sink_dshot.cpp`: the
  sink of section 5.
- `firmware_app.hpp`: the member swaps `MotorSinkNull` ->
  `MotorSinkDshot`; `motor_sink_null.hpp` is deleted.
- `tests/`: the encode-function unit test.

Nothing changes in flight-core, protocol, transport, hub or pages: the
whole feature is one platform service growing a real backend.

## 7. Bench procedure

Propellers off, in this order:

1. Flash (SWD or OTA), power the ESC from the battery. Expect the
   BLHeli_32 power-up beeps, then the arm tone once the firmware
   streams zeros.
2. Hub + pages up, board node visible, transmitter widget connected.
3. Arm from the widget, raise the throttle slider slightly: all four
   motors spin. Check the status log line reports the four commands.
4. Map check: each motor position responds (M1 rear right through M4
   front left); spin directions per the X-quad layout, fixed in the
   BLHeli suite if wrong.
5. Safety checks, each must cut the motors: kill switch on the widget;
   closing the page (500 ms fail-safe); and arming must be refused
   while an OTA trial image is unconfirmed.

## 8. Next steps

Not designed here, listed with their landing zone so none turns into an
implicit promise. Common policy: each one only adds fields to the
telemetry; virtual drones simply do not produce them, and absence is a
normal value for a consumer (the pattern the barometer already set).
No plant modeling, no priority implied by the order.

- **Bidirectional DShot (eRPM).** The frame goes out inverted, the pin
  switches to input after each frame, and the ESC answers about 30 us
  later on the same wire: a 21-bit GCR-encoded response at 5/4 of the
  bitrate carrying the eRPM period and a CRC. eRPM / pole pairs =
  mechanical RPM, per motor, at loop rate. This is the input of RPM
  notch filtering on the gyro, its real payoff; implementation cost is
  the output-compare to input-capture dance plus capture DMA and GCR
  decoding. Revisit when filtering work starts.
- **ESC telemetry UART (PA3, USART2 RX, 115200).** Setting the
  telemetry bit in one motor's DShot frame makes that ESC send a
  10-byte KISS-style frame on the shared TLM wire: temperature (deg C),
  voltage (10 mV), current (10 mA), consumed mAh, eRPM/100, CRC8.
  Round-robin over the four ESCs, a few tens of Hz per motor: health
  data per motor, not a control input.
- **Current sense ADC (PC2, ADC1_IN14).** The Velox current output is
  an analog voltage meant for direct wiring into a 3.3 V FC ADC input,
  which is how it is wired. The scale (A per volt) is a calibration
  knob, checked against the UART telemetry current, never a datasheet
  constant. Lands as the battery current in telemetry.
