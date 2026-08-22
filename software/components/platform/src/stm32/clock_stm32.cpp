#include "platform_stm32/clock_stm32.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_APB1ENR_TIM2EN = 1U << 0U;

        /// APB1 timer clock (2 x the 42 MHz bus clock) down to 1 MHz.
        constexpr std::uint32_t TIM_PSC_84MHZ_TO_1MHZ = 84U - 1U;

        constexpr std::uint32_t TIM_CR1_CEN = 1U << 0U;
        constexpr std::uint32_t TIM_EGR_UG = 1U << 0U;
    } // namespace

    void ClockStm32::init()
    {
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_TIM2EN;
        TIM2->PSC = TIM_PSC_84MHZ_TO_1MHZ;
        TIM2->ARR = 0xFFFFFFFFU;
        TIM2->EGR = TIM_EGR_UG; // latch the prescaler now
        TIM2->CR1 = TIM_CR1_CEN;
    }

    std::uint64_t ClockStm32::nowUs()
    {
        const std::uint32_t count = TIM2->CNT;
        if (count < m_lastCount)
        {
            ++m_wraps;
        }
        m_lastCount = count;
        return (static_cast<std::uint64_t>(m_wraps) << 32U) | count;
    }
} // namespace mark4
