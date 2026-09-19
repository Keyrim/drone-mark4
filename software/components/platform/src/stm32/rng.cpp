#include "platform_stm32/rng.hpp"

#include <stm32f405xx.h>

#include "platform_stm32/board.hpp"

namespace mark4
{
    namespace
    {
        /// Spins on DRDY before the fallback: the peripheral needs a few
        /// dozen cycles of its own clock for the first word, and nothing
        /// here may block a boot.
        constexpr std::uint32_t DRDY_TIMEOUT_LOOPS = 100000U;

    } // namespace

    std::uint32_t randomBootId()
    {
        RCC->AHB2ENR = RCC->AHB2ENR | RCC_AHB2ENR_RNGEN;
        RNG->CR = RNG->CR | RNG_CR_RNGEN;
        for (std::uint32_t loop = 0U; loop < DRDY_TIMEOUT_LOOPS; ++loop)
        {
            if ((RNG->SR & RNG_SR_DRDY) != 0U)
            {
                const std::uint32_t word = RNG->DR;
                return word == 0U ? 1U : word;
            }
        }
        // No analog seed: the cycle counter has run for as long as this boot
        // took, which differs from one boot to the next, and the unique id
        // keeps two boards apart. The transport only ever compares this
        // number with itself.
        const std::uint32_t mixed = boardNodeId() ^ DWT->CYCCNT;
        return mixed == 0U ? 1U : mixed;
    }
} // namespace mark4
