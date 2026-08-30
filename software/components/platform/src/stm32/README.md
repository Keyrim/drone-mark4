# software/components/platform/src/stm32

The board variant: `SensorSourceStm32` paces the loop on TIM3 at 500 Hz
and reads the MPU6050 (every tick) and the BMP581 (every third tick) over
the polled I2C1 master; `ClockStm32`, `Uart1Stream`, the RTT sink, the
internal flash and the OTA slots complete it.

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

## Bus timing on a failure

`I2cBus` polls every flag with a bounded loop (50000 iterations, no clock),
so a stuck bus degrades into failed transfers, never a hang. An unplugged
sensor NACKs its address within one byte time and the read returns in tens
of microseconds. The worst case is a slave holding the bus: one exhausted
flag wait plus the BUSY wait of the abort, on the order of 1 to 3 ms at
168 MHz, which overruns the 2 ms tick once (counted by `overruns()`) but
recovers on the next. No retry or bus recovery is attempted here.
