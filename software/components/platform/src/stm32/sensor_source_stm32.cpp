#include "platform_stm32/sensor_source_stm32.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_APB1ENR_TIM3EN = 1U << 1U;
        constexpr std::uint32_t TIM3_IRQ_NUMBER = 29U;

        /// APB1 timer clock (2 x the 42 MHz bus clock) down to 1 MHz.
        constexpr std::uint32_t TIM_PSC_84MHZ_TO_1MHZ = 84U - 1U;

        constexpr std::uint32_t TIM_CR1_CEN = 1U << 0U;
        constexpr std::uint32_t TIM_DIER_UIE = 1U << 0U;
        constexpr std::uint32_t TIM_EGR_UG = 1U << 0U;

        constexpr std::uint32_t US_PER_S = 1000000U;

        /// Ticks delivered by the TIM3 interrupt, consumed by waitFrame().
        volatile std::uint32_t g_ticks = 0U;
    } // namespace

    void SensorSourceStm32::init()
    {
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_TIM3EN;
        TIM3->PSC = TIM_PSC_84MHZ_TO_1MHZ;
        TIM3->ARR = (US_PER_S / FRAME_RATE_HZ) - 1U;
        TIM3->EGR = TIM_EGR_UG; // latch the prescaler now
        TIM3->SR = 0U;          // the UG above set UIF, do not fire early
        TIM3->DIER = TIM_DIER_UIE;
        NVIC_ISER[TIM3_IRQ_NUMBER / 32U] = 1U << (TIM3_IRQ_NUMBER % 32U);
        TIM3->CR1 = TIM_CR1_CEN;
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

        Mpu6050Sample sample{};
        if (m_imu.readSample(sample))
        {
            m_lastSample = sample;
        }
        else
        {
            ++m_readFailures;
            sample = m_lastSample;
        }

        frameOut.timestampUs = m_clock.nowUs();
        // Chip axes taken as body axes for now: the breakout sits flat, z
        // up; the mapping gets a proper calibration once the mounting is
        // final. TODO(tmagne): axis map from the mechanical mounting.
        for (std::uint32_t axis = 0U; axis < 3U; ++axis)
        {
            frameOut.gyroRadS[axis] =
                static_cast<float>(sample.gyro[axis]) * Mpu6050::GYRO_RADS_PER_LSB;
            frameOut.accelMps2[axis] =
                static_cast<float>(sample.accel[axis]) * Mpu6050::ACCEL_MPS2_PER_LSB;
        }
        // The barometer publishes at its own rate (80 Hz) and is read once
        // per tick, so the frame always carries its latest solution; 0.0
        // until the first one lands, and for good if the chip never
        // came up.
        m_baro.update();
        frameOut.baroPa = m_baro.pressurePa();
        frameOut.rc = RcInput{}; // no receiver yet: kill switch engaged
        return FrameWait::FRAME;
    }
} // namespace mark4

/// TIM3 update interrupt: acknowledge and hand one tick to waitFrame().
extern "C" void TIM3_IRQHandler(void)
{
    mark4::TIM3->SR = 0U;
    mark4::g_ticks = mark4::g_ticks + 1U;
}
