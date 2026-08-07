#include "platform_stm32/board.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_CR_HSEON = 1U << 16U;
        constexpr std::uint32_t RCC_CR_HSERDY = 1U << 17U;
        constexpr std::uint32_t RCC_CR_PLLON = 1U << 24U;
        constexpr std::uint32_t RCC_CR_PLLRDY = 1U << 25U;

        // Main PLL: 8 MHz HSE / M=4 -> 2 MHz comparison frequency, * N=168
        // -> 336 MHz VCO, / P=2 (PLLP bits left at 00) -> 168 MHz SYSCLK,
        // / Q=7 -> 48 MHz for the USB/SDIO domain (unused but kept in spec).
        constexpr std::uint32_t PLL_M = 4U;
        constexpr std::uint32_t PLL_N = 168U;
        constexpr std::uint32_t PLL_Q = 7U;
        constexpr std::uint32_t PLL_N_SHIFT = 6U;
        constexpr std::uint32_t PLL_Q_SHIFT = 24U;
        constexpr std::uint32_t RCC_PLLCFGR_SRC_HSE = 1U << 22U;

        // Bus prescalers: AHB /1 (168 MHz), APB1 /4 (42 MHz, its 45 MHz
        // max), APB2 /2 (84 MHz, its 90 MHz max).
        constexpr std::uint32_t RCC_CFGR_PPRE1_DIV4 = 5U << 10U;
        constexpr std::uint32_t RCC_CFGR_PPRE2_DIV2 = 4U << 13U;
        constexpr std::uint32_t RCC_CFGR_SW_MASK = 3U;
        constexpr std::uint32_t RCC_CFGR_SW_PLL = 2U;
        constexpr std::uint32_t RCC_CFGR_SWS_MASK = 3U << 2U;
        constexpr std::uint32_t RCC_CFGR_SWS_PLL = 2U << 2U;

        // 5 wait states (168 MHz at 3.3 V), prefetch and both caches on.
        // The voltage regulator stays on its reset scale 1, which allows
        // 168 MHz without touching PWR.
        constexpr std::uint32_t FLASH_ACR_LATENCY_5WS = 5U;
        constexpr std::uint32_t FLASH_ACR_PRFTEN = 1U << 8U;
        constexpr std::uint32_t FLASH_ACR_ICEN = 1U << 9U;
        constexpr std::uint32_t FLASH_ACR_DCEN = 1U << 10U;

        /// Poll budget for HSE/PLL ready flags: worst-case crystal startup
        /// is a few ms, this is well above 100 ms on the 16 MHz boot clock.
        constexpr std::uint32_t READY_TIMEOUT_LOOPS = 500000U;

        constexpr std::uint32_t DEMCR_TRCENA = 1U << 24U;
        constexpr std::uint32_t DWT_CTRL_CYCCNTENA = 1U << 0U;

        constexpr std::uint32_t RCC_AHB1ENR_GPIOCEN = 1U << 2U;
        constexpr std::uint32_t LED1_PIN = 13U;
        constexpr std::uint32_t LED2_PIN = 14U;
        constexpr std::uint32_t GPIO_MODER_OUTPUT = 1U;

        /// Core clock the rest of the code trusts; HSI until the PLL is up.
        std::uint32_t g_coreClockHz = HSI_CLOCK_HZ;

        /// @brief Polls a register until (reg & mask) == expected.
        /// @param reg register to poll
        /// @param mask bits compared
        /// @param expected value the masked bits must reach
        /// @return true when the value was reached before the poll budget ran out
        bool waitMasked(volatile std::uint32_t &reg, std::uint32_t mask, std::uint32_t expected)
        {
            for (std::uint32_t loop = 0U; loop < READY_TIMEOUT_LOOPS; ++loop)
            {
                if ((reg & mask) == expected)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool initSystemClock()
    {
        RCC->CR = RCC->CR | RCC_CR_HSEON;
        if (!waitMasked(RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY))
        {
            return false;
        }

        FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
        RCC->CFGR = RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

        RCC->PLLCFGR =
            PLL_M | (PLL_N << PLL_N_SHIFT) | RCC_PLLCFGR_SRC_HSE | (PLL_Q << PLL_Q_SHIFT);
        RCC->CR = RCC->CR | RCC_CR_PLLON;
        if (!waitMasked(RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY))
        {
            return false;
        }

        RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_MASK) | RCC_CFGR_SW_PLL;
        if (!waitMasked(RCC->CFGR, RCC_CFGR_SWS_MASK, RCC_CFGR_SWS_PLL))
        {
            return false;
        }

        g_coreClockHz = CORE_CLOCK_HZ;
        return true;
    }

    std::uint32_t coreClockHz()
    {
        return g_coreClockHz;
    }

    void initCycleCounter()
    {
        *DEMCR = *DEMCR | DEMCR_TRCENA;
        DWT->CYCCNT = 0U;
        DWT->CTRL = DWT->CTRL | DWT_CTRL_CYCCNTENA;
    }

    void delayMs(std::uint32_t milliseconds)
    {
        // One millisecond per inner wait keeps the 32-bit cycle math exact
        // for arbitrarily long delays (the counter wraps every ~25 s).
        const std::uint32_t cyclesPerMs = g_coreClockHz / 1000U;
        for (std::uint32_t elapsed = 0U; elapsed < milliseconds; ++elapsed)
        {
            const std::uint32_t start = DWT->CYCCNT;
            while (DWT->CYCCNT - start < cyclesPerMs)
            {
            }
        }
    }

    void initLeds()
    {
        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPIOCEN;

        constexpr std::uint32_t MODE_MASK = (3U << (2U * LED1_PIN)) | (3U << (2U * LED2_PIN));
        constexpr std::uint32_t MODE_OUTPUT =
            (GPIO_MODER_OUTPUT << (2U * LED1_PIN)) | (GPIO_MODER_OUTPUT << (2U * LED2_PIN));
        GPIOC->MODER = (GPIOC->MODER & ~MODE_MASK) | MODE_OUTPUT;
    }

    void toggleLed1()
    {
        GPIOC->ODR = GPIOC->ODR ^ (1U << LED1_PIN);
    }

    void setLed1(bool on)
    {
        // BSRR: low half sets the pin, high half resets it, atomically.
        GPIOC->BSRR = on ? (1U << LED1_PIN) : (1U << (LED1_PIN + 16U));
    }

    void setLed2(bool on)
    {
        GPIOC->BSRR = on ? (1U << LED2_PIN) : (1U << (LED2_PIN + 16U));
    }
} // namespace mark4
