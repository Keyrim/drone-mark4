#pragma once

/// @file
/// @brief MPU6050 IMU driver over I2C: identification, configuration and
///        raw sample bursts.

#include <cstdint>

#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/sensor_health.hpp"

namespace mark4
{
    /// One raw sample, sign and axis order exactly as the chip reports
    /// them. Scaling to SI units is the caller's business, with the
    /// per-LSB constants below.
    struct Mpu6050Sample
    {
        std::int16_t accel[3];    ///< accelerometer X/Y/Z counts
        std::int16_t gyro[3];     ///< gyroscope X/Y/Z counts
        std::int16_t temperature; ///< die temperature counts
    };

    /// MPU6050 at its default address, configured for the widest ranges:
    /// +/-2000 deg/s (hand throws tumble well below the saturation) and
    /// +/-16 g (catch impacts spike hard). Blocking reads, no interrupt
    /// line: the caller paces the sampling.
    class Mpu6050
    {
      public:
        /// 7-bit address with the AD0 pin low.
        static constexpr std::uint8_t I2C_ADDRESS = 0x68U;

        /// Gyroscope scale at +/-2000 deg/s [rad/s per count].
        static constexpr float GYRO_RADS_PER_LSB = (2000.0F / 32768.0F) * (3.14159265F / 180.0F);

        /// Accelerometer scale at +/-16 g [m/s^2 per count].
        static constexpr float ACCEL_MPS2_PER_LSB = (16.0F / 32768.0F) * 9.80665F;

        /// Failure duration after which the WARN of the first failed read
        /// becomes an ERROR [us]: the flight core's own fault horizon.
        static constexpr std::uint64_t FAULT_HORIZON_US = 20000U;

        /// @param bus initialized I2C bus the chip sits on
        explicit Mpu6050(I2cBus &bus);

        /// @brief Checks WHO_AM_I, wakes the chip on the gyro PLL clock,
        ///        selects the ranges above and a 44 Hz low-pass, and opens
        ///        the auxiliary bus bypass so devices behind XDA/XCL appear
        ///        on the main bus. Failures are logged over RTT.
        /// @return true when the chip answered and took the configuration
        bool init();

        /// @brief Reads one accelerometer + temperature + gyroscope burst.
        ///        The outcome feeds the health tracker, which logs the
        ///        transitions (first failure, lasting outage, recovery).
        /// @param sample receives the decoded raw counts
        /// @param nowUs instant of the read [us]
        /// @return true when the burst transfer completed
        bool readSample(Mpu6050Sample &sample, std::uint64_t nowUs);

      private:
        I2cBus &m_bus;         ///< transport, not owned
        SensorHealth m_health; ///< read outcomes and their logs
    };
} // namespace mark4
