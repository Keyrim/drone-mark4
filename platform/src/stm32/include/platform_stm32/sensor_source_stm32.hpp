#pragma once

/// @file
/// @brief Timer-paced sensor source: the single wait point on target.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_stm32/clock_stm32.hpp"
#include "platform_stm32/mpu6050.hpp"
#include "platform_stm32/ms5611.hpp"

namespace mark4
{
    /// Produces one SensorFrame per TIM3 tick (the MPU interrupt line is
    /// not wired on this board, so a timer paces the loop). waitFrame()
    /// sleeps on WFI between ticks. RC is not received yet: frames carry
    /// the safe defaults, kill switch engaged.
    class SensorSourceStm32 final : public AbsSensorSource
    {
      public:
        /// Loop rate; the 400 kHz I2C budget per tick is a 14-byte IMU
        /// burst (~0.4 ms), well inside the 2 ms period.
        static constexpr std::uint32_t FRAME_RATE_HZ = 500U;

        /// @param imu initialized IMU the samples come from
        /// @param baro initialized barometer, pumped once per tick
        /// @param clock started clock stamping the frames
        SensorSourceStm32(Mpu6050 &imu, Ms5611 &baro, ClockStm32 &clock)
            : m_imu(imu),
              m_baro(baro),
              m_clock(clock)
        {
        }

        /// @brief Starts TIM3 at FRAME_RATE_HZ and enables its interrupt.
        ///        Assumes the 84 MHz APB1 timer clock set by
        ///        initSystemClock().
        void init();

        /// @brief Sleeps until the next tick, then reads the IMU. On an
        ///        I2C failure the previous sample is reused and the frame
        ///        is delivered anyway: one glitch must not stop the loop.
        /// @param[out] frameOut filled frame
        /// @return always true, the source never runs dry
        bool waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return ticks that fired while the previous frame was still
        ///         being processed (deadline misses)
        [[nodiscard]] std::uint32_t overruns() const
        {
            return m_overruns;
        }

        /// @return IMU bursts that failed and were papered over with the
        ///         previous sample
        [[nodiscard]] std::uint32_t readFailures() const
        {
            return m_readFailures;
        }

      private:
        Mpu6050 &m_imu;                     ///< sample producer, not owned
        Ms5611 &m_baro;                     ///< pressure producer, not owned
        ClockStm32 &m_clock;                ///< frame timestamps, not owned
        Mpu6050Sample m_lastSample{};       ///< reused when a burst fails
        std::uint32_t m_consumedTicks = 0U; ///< ticks turned into frames
        std::uint32_t m_overruns = 0U;      ///< missed deadlines
        std::uint32_t m_readFailures = 0U;  ///< failed IMU bursts
    };
} // namespace mark4
