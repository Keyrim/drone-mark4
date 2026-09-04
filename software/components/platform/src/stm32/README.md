# software/components/platform/src/stm32

The board variant: `SensorSourceStm32` paces the loop on TIM6 at 500 Hz
and reads the MPU6050 (every tick) and the BMP581 (every third tick) over
the polled I2C1 master; `MotorSinkDshot` clocks the four DShot600 frames
out of TIM3 through one DMA burst; `ClockStm32`, `Uart1Stream`, the ESC
telemetry wire, the RTT sink, the internal flash and the OTA slots
complete it.

## Frame validity

- `imuValid` is the outcome of this tick's MPU6050 burst. A failed burst
  (NACK on an unplugged sensor, a bus timeout) delivers the frame with the
  flag false and gyro / accel at zero, never the previous sample.
- `baroValid` is true while the BMP581 holds a plausible solution younger
  than `Bmp581::FRESH_MAX_AGE_US` (50 ms). The chip publishes at 80 Hz
  and is read every 6 ms, so most frames carry a *held* solution; held but
  fresh counts as valid, and the frame says "no baro" (flag false, 0 Pa)
  past the window or while the chip never came up.

Each driver feeds a `SensorHealth` tracker with its own log module
(`platform/imu`, `platform/baro`) that logs transitions, never ticks: WARN
`read failed, sample marked invalid` on the first failure after a good run,
ERROR `no sample for N ms` once the outage lasted the horizon (20 ms for the
IMU, the flight core's fault threshold; 500 ms for the baro) and again every
5 s while it lasts, INFO `recovered after N failed reads` on the first
success. The firmware logs `flight/core` ERROR `FAULT: imu lost in flight,
motors cut` once when the core latches its fault.

## ESC telemetry

The four ESCs of the 4-in-1 share one telemetry wire into PA3 (USART2_RX,
115200 baud, receive only; nothing is ever sent to them on it). An ESC
answers with a 10-byte KISS frame (temperature, voltage, current, mAh,
eRPM, CRC-8) only when the DShot frame it just received carried the
telemetry bit, so `EscTelemetry` keeps a single request outstanding:
once per output frame the composition drains the USART into `feed()`,
then `nextRequest()` names the motor whose frame must carry the bit and
`MotorSinkDshot::requestTelemetry()` sets it in the next `push()`, that
channel only. A complete frame with a good CRC records the sample and
passes the turn to the next ESC; a request unanswered after
`REQUEST_TIMEOUT_US` (20 ms) is counted and skipped; bytes arriving with
no request outstanding are counted as stray and dropped, which is also
how the stream resynchronizes after a corrupted frame. With ESCs that
answer within the 2 ms frame every one of them is refreshed at about
125 Hz.

The samples and the three wire counters are telemetry measures
(`esc/...`, see `components/telemetry/README.md`), the codec and the
sequencer are register-free and pinned by a desktop test, and the USART
itself is `esc_uart.cpp`: a 64-byte circular DMA ring (DMA1 stream 5)
read by NDTR, no interrupt, like the USART1 receive side. An ESC that is
not powered simply never answers: every request times out, the counter
climbs, nothing else happens. The firmware logs `platform/esc` INFO
`esc N reporting` when an ESC's first frame arrives and WARN `esc N
silent for 1000 ms` when one that was reporting stops, checked once per
status window.

## Bus timing on a failure

`I2cBus` polls every flag with a bounded loop (50000 iterations, no clock),
so a stuck bus degrades into failed transfers, never a hang. An unplugged
sensor NACKs its address within one byte time and the read returns in tens
of microseconds. The worst case is a slave holding the bus: one exhausted
flag wait plus the BUSY wait of the abort, on the order of 1 to 3 ms at
168 MHz, which overruns the 2 ms tick once (counted by `overruns()`) but
recovers on the next. No retry or bus recovery is attempted here.
