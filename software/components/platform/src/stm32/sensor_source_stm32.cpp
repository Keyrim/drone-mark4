#include "platform_stm32/sensor_source_stm32.hpp"

#include <cstdint>

#include <stm32f405xx.h>

#include "platform_stm32/board.hpp"

namespace mark4
{
    namespace
    {
        // TIM6, a basic timer with no output pins: TIM3's four channels
        // drive the motors, so the pacer cannot share it. TIM6 sits on the
        // same APB1 clock and takes the same prescaler/reload; its interrupt
        // is shared with the DAC, which this board never uses.

        /// APB1 timer clock down to 1 MHz.
        constexpr std::uint32_t TIM_PSC_TO_1MHZ = (APB1_TIMER_CLOCK_HZ / 1000000U) - 1U;

        constexpr std::uint32_t US_PER_S = 1000000U;

        /// Ticks delivered by the TIM3 interrupt, consumed by waitFrame().
        volatile std::uint32_t g_ticks = 0U;
    } // namespace

    void SensorSourceStm32::init()
    {
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_TIM6EN;
        TIM6->PSC = TIM_PSC_TO_1MHZ;
        TIM6->ARR = (US_PER_S / FRAME_RATE_HZ) - 1U;
        TIM6->EGR = TIM_EGR_UG; // latch the prescaler now
        TIM6->SR = 0U;          // the UG above set UIF, do not fire early
        TIM6->DIER = TIM_DIER_UIE;
        NVIC_EnableIRQ(TIM6_DAC_IRQn);
        TIM6->CR1 = TIM_CR1_CEN;
    }

    FrameWait SensorSourceStm32::waitFrame(mark4::SensorFrame &frameOut)
    {
        // Accepted race: a tick between the test and the WFI sleeps one
        // extra tick (the next interrupt still wakes the core), counted as
        // an overrun below. The counter itself never loses a tick.
        while (g_ticks == m_consumedTicks)
        {
            __asm volatile("wfi");
        }
        const std::uint32_t ticks = g_ticks;
        m_overruns += (ticks - m_consumedTicks) - 1U;
        m_consumedTicks = ticks;

        const std::uint64_t nowUs = m_clock.nowUs();
        frameOut.timestampUs = nowUs;

        // A failed burst is a frame without an IMU, zeros and the flag down:
        // replaying the previous sample would have the core integrate a
        // frozen gyro as if the drone had stopped moving.
        Mpu6050Sample sample{};
        frameOut.imuValid = m_imu.readSample(sample, nowUs);
        if (!frameOut.imuValid)
        {
            ++m_readFailures;
            sample = Mpu6050Sample{};
        }
        // Axis map from the mechanical mounting: the chip sits flat, z up,
        // yawed +90 deg on the frame (chip x points to body left, chip y to
        // body rear). Body frame is x forward, y left, z up, so
        // body = (-chip y, +chip x, +chip z) for both sensors.
        frameOut.gyroRadS[0] = -static_cast<float>(sample.gyro[1]) * Mpu6050::GYRO_RADS_PER_LSB;
        frameOut.gyroRadS[1] = static_cast<float>(sample.gyro[0]) * Mpu6050::GYRO_RADS_PER_LSB;
        frameOut.gyroRadS[2] = static_cast<float>(sample.gyro[2]) * Mpu6050::GYRO_RADS_PER_LSB;
        frameOut.accelMps2[0] = -static_cast<float>(sample.accel[1]) * Mpu6050::ACCEL_MPS2_PER_LSB;
        frameOut.accelMps2[1] = static_cast<float>(sample.accel[0]) * Mpu6050::ACCEL_MPS2_PER_LSB;
        frameOut.accelMps2[2] = static_cast<float>(sample.accel[2]) * Mpu6050::ACCEL_MPS2_PER_LSB;
        // The barometer publishes at its own rate (80 Hz) and is read every
        // few ticks, so most frames carry a held solution: it counts as
        // valid while it is younger than the driver's freshness window,
        // and the frame says "no baro" past it (or if the chip never came
        // up).
        m_baro.update(nowUs);
        frameOut.baroValid = m_baro.fresh(nowUs);
        frameOut.baroPa = frameOut.baroValid ? m_baro.pressurePa() : 0.0f;
        frameOut.rc = RcInput{}; // no receiver yet: kill switch engaged
        return FrameWait::FRAME;
    }
} // namespace mark4

/// TIM6 update interrupt: acknowledge and hand one tick to waitFrame().
extern "C" void TIM6_DAC_IRQHandler(void)
{
    TIM6->SR = 0U;
    mark4::g_ticks = mark4::g_ticks + 1U;
}
