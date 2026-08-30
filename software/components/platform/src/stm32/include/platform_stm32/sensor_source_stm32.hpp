#pragma once

/// @file
/// @brief Timer-paced sensor source: the single wait point on target.

#include <cstdint>

#include "platform/sensor_source.hpp"
#include "platform_stm32/bmp581.hpp"
#include "platform_stm32/clock_stm32.hpp"
#include "platform_stm32/mpu6050.hpp"

namespace mark4
{
    /// Produces one SensorFrame per TIM3 tick (the MPU interrupt line is
    /// not wired on this board, so a timer paces the loop). waitFrame()
    /// sleeps on WFI between ticks. RC is not received yet: frames carry
    /// the safe defaults, kill switch engaged.
    ///
    /// Single instance only: the TIM3 tick counter consumed by waitFrame()
    /// is file-scope state shared with the interrupt handler, so a second
    /// instance would consume the same ticks. Acceptable for a service
    /// that owns one hardware timer; it just cannot be duplicated.
    class SensorSourceStm32 final : public AbsSensorSource
    {
      public:
        /// Loop rate; the 400 kHz I2C budget per tick is a 14-byte IMU
        /// burst (~0.4 ms), well inside the 2 ms period.
        static constexpr std::uint32_t FRAME_RATE_HZ = 500U;

        /// @param imu initialized IMU the samples come from
        /// @param baro barometer read once per tick; one that failed to
        ///        come up stays inert and reports no pressure
        /// @param clock started clock stamping the frames
        SensorSourceStm32(Mpu6050 &imu, Bmp581 &baro, ClockStm32 &clock)
            : m_imu(imu),
              m_baro(baro),
              m_clock(clock)
        {
        }

        /// @brief Starts TIM3 at FRAME_RATE_HZ and enables its interrupt.
        ///        Assumes the 84 MHz APB1 timer clock set by
        ///        initSystemClock().
        void init();

        /// @brief Sleeps until the next tick, then reads the IMU and
        ///        updates the baro. The frame is delivered whatever the bus
        ///        did: a failed IMU burst leaves imuValid false and the
        ///        gyro / accel at zero, never the previous sample; baroValid
        ///        is true when the baro holds a plausible solution younger
        ///        than Bmp581::FRESH_MAX_AGE_US, and baroPa is 0 otherwise.
        ///        A tick firing between the counter test and the WFI makes
        ///        the sleep last one extra tick, counted as an overrun;
        ///        the race is accepted - it costs one 2 ms hiccup, never a
        ///        drifting tick count.
        /// @param[out] frameOut filled frame
        /// @return always FRAME, the source never runs dry
        FrameWait waitFrame(mark4::SensorFrame &frameOut) override;

        /// @return ticks that fired while the previous frame was still
        ///         being processed (deadline misses)
        [[nodiscard]] std::uint32_t overruns() const
        {
            return m_overruns;
        }

        /// @return IMU bursts that failed, each delivered as an invalid frame
        [[nodiscard]] std::uint32_t readFailures() const
        {
            return m_readFailures;
        }

      private:
        Mpu6050 &m_imu;                     ///< sample producer, not owned
        Bmp581 &m_baro;                     ///< pressure producer, not owned
        ClockStm32 &m_clock;                ///< frame timestamps, not owned
        std::uint32_t m_consumedTicks = 0U; ///< ticks turned into frames
        std::uint32_t m_overruns = 0U;      ///< missed deadlines
        std::uint32_t m_readFailures = 0U;  ///< failed IMU bursts
    };
} // namespace mark4
