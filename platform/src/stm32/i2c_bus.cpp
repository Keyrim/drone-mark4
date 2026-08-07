#include "platform_stm32/i2c_bus.hpp"

#include <cstdint>

#include "registers.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint32_t RCC_AHB1ENR_GPIOBEN = 1U << 1U;
        constexpr std::uint32_t RCC_APB1ENR_I2C1EN = 1U << 21U;

        constexpr std::uint32_t SCL_PIN = 8U;
        constexpr std::uint32_t SDA_PIN = 9U;
        constexpr std::uint32_t GPIO_AF4 = 4U;
        constexpr std::uint32_t GPIO_MODER_AF = 2U;

        constexpr std::uint32_t I2C_CR1_PE = 1U << 0U;
        constexpr std::uint32_t I2C_CR1_START = 1U << 8U;
        constexpr std::uint32_t I2C_CR1_STOP = 1U << 9U;
        constexpr std::uint32_t I2C_CR1_SWRST = 1U << 15U;
        constexpr std::uint32_t I2C_SR1_SB = 1U << 0U;
        constexpr std::uint32_t I2C_SR1_ADDR = 1U << 1U;
        constexpr std::uint32_t I2C_SR1_AF = 1U << 10U;
        constexpr std::uint32_t I2C_SR2_BUSY = 1U << 1U;

        // Standard mode timing on the 42 MHz APB1 clock: CCR holds the SCL
        // half-period (42 MHz / (2 * 100 kHz)), TRISE the 1000 ns maximum
        // rise time plus one, both from RM0090.
        constexpr std::uint32_t APB1_CLOCK_MHZ = 42U;
        constexpr std::uint32_t I2C_CCR_100KHZ = 210U;
        constexpr std::uint32_t I2C_TRISE_100KHZ = APB1_CLOCK_MHZ + 1U;

        /// Poll budget per flag: a few SCL periods at 100 kHz, with margin.
        constexpr std::uint32_t FLAG_TIMEOUT_LOOPS = 50000U;

        /// @brief Polls a register until (reg & mask) == expected.
        /// @param reg register to poll
        /// @param mask bits compared
        /// @param expected value the masked bits must reach
        /// @return true when the value was reached before the poll budget ran out
        bool waitMasked(volatile std::uint32_t &reg, std::uint32_t mask, std::uint32_t expected)
        {
            for (std::uint32_t loop = 0U; loop < FLAG_TIMEOUT_LOOPS; ++loop)
            {
                if ((reg & mask) == expected)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    bool I2cBus::init()
    {
        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPIOBEN;
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_I2C1EN;

        GPIOB->OTYPER = GPIOB->OTYPER | (1U << SCL_PIN) | (1U << SDA_PIN);

        constexpr std::uint32_t AF_MASK = (0xFU << (4U * (SCL_PIN - 8U))) | //
                                          (0xFU << (4U * (SDA_PIN - 8U)));
        constexpr std::uint32_t AF_I2C =
            (GPIO_AF4 << (4U * (SCL_PIN - 8U))) | (GPIO_AF4 << (4U * (SDA_PIN - 8U)));
        GPIOB->AFR[1] = (GPIOB->AFR[1] & ~AF_MASK) | AF_I2C;

        constexpr std::uint32_t MODE_MASK = (3U << (2U * SCL_PIN)) | (3U << (2U * SDA_PIN));
        constexpr std::uint32_t MODE_AF =
            (GPIO_MODER_AF << (2U * SCL_PIN)) | (GPIO_MODER_AF << (2U * SDA_PIN));
        GPIOB->MODER = (GPIOB->MODER & ~MODE_MASK) | MODE_AF;

        // Software reset first: it clears a BUSY flag latched from line
        // glitches during power-up, a known F4 I2C trap.
        I2C1->CR1 = I2C_CR1_SWRST;
        I2C1->CR1 = 0U;
        I2C1->CR2 = APB1_CLOCK_MHZ;
        I2C1->CCR = I2C_CCR_100KHZ;
        I2C1->TRISE = I2C_TRISE_100KHZ;
        I2C1->CR1 = I2C_CR1_PE;

        m_ready = waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U);
        return m_ready;
    }

    bool I2cBus::probe(std::uint8_t address)
    {
        if (!m_ready || !waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U))
        {
            return false;
        }

        I2C1->CR1 = I2C1->CR1 | I2C_CR1_START;
        if (!waitMasked(I2C1->SR1, I2C_SR1_SB, I2C_SR1_SB))
        {
            I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
            return false;
        }

        I2C1->DR = static_cast<std::uint32_t>(address) << 1U; // write direction

        bool acked = false;
        for (std::uint32_t loop = 0U; loop < FLAG_TIMEOUT_LOOPS; ++loop)
        {
            const std::uint32_t sr1 = I2C1->SR1;
            if ((sr1 & I2C_SR1_ADDR) != 0U)
            {
                acked = true;
                break;
            }
            if ((sr1 & I2C_SR1_AF) != 0U)
            {
                break;
            }
        }

        if (acked)
        {
            // ADDR is cleared by reading SR1 then SR2.
            const std::uint32_t sr1 = I2C1->SR1;
            const std::uint32_t sr2 = I2C1->SR2;
            static_cast<void>(sr1);
            static_cast<void>(sr2);
        }
        else
        {
            I2C1->SR1 = ~I2C_SR1_AF; // AF is cleared by writing it to zero
        }

        I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
        static_cast<void>(waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U));
        return acked;
    }
} // namespace mark4
