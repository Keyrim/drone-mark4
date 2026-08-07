#include "platform_stm32/i2c_bus.hpp"

#include <cstdint>

#include "platform_stm32/board.hpp"
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
        constexpr std::uint32_t GPIO_MODER_OUTPUT = 1U;
        constexpr std::uint32_t GPIO_MODER_AF = 2U;

        /// SCL pulses freeing a slave stuck mid-transaction (one byte plus
        /// ACK is the worst case before it releases SDA).
        constexpr std::uint32_t RECOVERY_CLOCKS = 9U;

        /// Half-period of the recovery clocking; leisurely on purpose.
        constexpr std::uint32_t RECOVERY_HALF_PERIOD_MS = 1U;

        constexpr std::uint32_t I2C_CR1_PE = 1U << 0U;
        constexpr std::uint32_t I2C_CR1_START = 1U << 8U;
        constexpr std::uint32_t I2C_CR1_STOP = 1U << 9U;
        constexpr std::uint32_t I2C_CR1_ACK = 1U << 10U;
        constexpr std::uint32_t I2C_CR1_POS = 1U << 11U;
        constexpr std::uint32_t I2C_CR1_SWRST = 1U << 15U;
        constexpr std::uint32_t I2C_SR1_SB = 1U << 0U;
        constexpr std::uint32_t I2C_SR1_ADDR = 1U << 1U;
        constexpr std::uint32_t I2C_SR1_BTF = 1U << 2U;
        constexpr std::uint32_t I2C_SR1_RXNE = 1U << 6U;
        constexpr std::uint32_t I2C_SR1_TXE = 1U << 7U;
        constexpr std::uint32_t I2C_SR1_AF = 1U << 10U;
        constexpr std::uint32_t I2C_SR2_BUSY = 1U << 1U;

        // Fast mode timing on the 42 MHz APB1 clock (all three breakout
        // chips are fast-mode capable, and a 14-byte IMU burst at 100 kHz
        // would eat 1.5 ms of a 2 ms loop budget). F/S=1, DUTY=0: SCL
        // period is 3 * CCR ticks, so 42 MHz / (3 * 35) = 400 kHz. TRISE
        // is the 300 ns fast-mode maximum rise time in ticks, plus one.
        constexpr std::uint32_t APB1_CLOCK_MHZ = 42U;
        constexpr std::uint32_t I2C_CCR_FS = 1U << 15U;
        constexpr std::uint32_t I2C_CCR_400KHZ = 35U;
        constexpr std::uint32_t I2C_TRISE_400KHZ = 13U;

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

        /// @brief Polls SR1 for an event flag, giving up on NACK.
        /// @param mask SR1 flag(s) waited for
        /// @return true when a flag rose; false on NACK or timeout
        bool waitEvent(std::uint32_t mask)
        {
            for (std::uint32_t loop = 0U; loop < FLAG_TIMEOUT_LOOPS; ++loop)
            {
                const std::uint32_t sr1 = I2C1->SR1;
                if ((sr1 & mask) != 0U)
                {
                    return true;
                }
                if ((sr1 & I2C_SR1_AF) != 0U)
                {
                    return false;
                }
            }
            return false;
        }

        /// @brief Releases the bus after a failed transfer: NACK flag
        ///        cleared, POS restored, STOP queued, BUSY waited out.
        void abortTransfer()
        {
            I2C1->SR1 = ~I2C_SR1_AF;
            I2C1->CR1 = (I2C1->CR1 | I2C_CR1_STOP) & ~I2C_CR1_POS;
            static_cast<void>(waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U));
        }

        /// @brief START (or repeated START) plus the address byte.
        /// @param address 7-bit device address
        /// @param readDirection true for a receive transfer
        /// @return true when the device acknowledged; ADDR left uncleared
        bool sendStart(std::uint8_t address, bool readDirection)
        {
            I2C1->CR1 = I2C1->CR1 | I2C_CR1_START;
            if (!waitEvent(I2C_SR1_SB))
            {
                return false;
            }
            I2C1->DR = (static_cast<std::uint32_t>(address) << 1U) | (readDirection ? 1U : 0U);
            return waitEvent(I2C_SR1_ADDR);
        }

        /// @brief Clears ADDR with the SR1-then-SR2 read sequence.
        void clearAddr()
        {
            const std::uint32_t sr1 = I2C1->SR1;
            const std::uint32_t sr2 = I2C1->SR2;
            static_cast<void>(sr1);
            static_cast<void>(sr2);
        }

        /// @brief Receive phase of a transfer, RM0090 master receiver
        ///        sequences: dedicated ACK/STOP choreography for 1 byte,
        ///        2 bytes (POS trick) and N >= 3 (NACK on N-2 after BTF).
        /// @param address 7-bit device address
        /// @param data receives the bytes read
        /// @param length number of bytes, at least 1
        /// @return true when the transfer completed
        bool receive(std::uint8_t address, std::uint8_t *data, std::uint32_t length)
        {
            if (length == 1U)
            {
                I2C1->CR1 = I2C1->CR1 & ~I2C_CR1_ACK;
                if (!sendStart(address, true))
                {
                    abortTransfer();
                    return false;
                }
                clearAddr();
                I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
                if (!waitEvent(I2C_SR1_RXNE))
                {
                    abortTransfer();
                    return false;
                }
                data[0] = static_cast<std::uint8_t>(I2C1->DR);
            }
            else if (length == 2U)
            {
                I2C1->CR1 = (I2C1->CR1 | I2C_CR1_POS) & ~I2C_CR1_ACK;
                if (!sendStart(address, true))
                {
                    abortTransfer();
                    return false;
                }
                clearAddr();
                if (!waitEvent(I2C_SR1_BTF))
                {
                    abortTransfer();
                    return false;
                }
                I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
                data[0] = static_cast<std::uint8_t>(I2C1->DR);
                data[1] = static_cast<std::uint8_t>(I2C1->DR);
                I2C1->CR1 = I2C1->CR1 & ~I2C_CR1_POS;
            }
            else
            {
                I2C1->CR1 = I2C1->CR1 | I2C_CR1_ACK;
                if (!sendStart(address, true))
                {
                    abortTransfer();
                    return false;
                }
                clearAddr();
                std::uint32_t index = 0U;
                for (std::uint32_t remaining = length; remaining > 3U; --remaining)
                {
                    if (!waitEvent(I2C_SR1_RXNE))
                    {
                        abortTransfer();
                        return false;
                    }
                    data[index] = static_cast<std::uint8_t>(I2C1->DR);
                    ++index;
                }
                // Three bytes left: after BTF, byte N-2 sits in DR and N-1
                // in the shift register; NACK then STOP around reading them.
                if (!waitEvent(I2C_SR1_BTF))
                {
                    abortTransfer();
                    return false;
                }
                I2C1->CR1 = I2C1->CR1 & ~I2C_CR1_ACK;
                data[index] = static_cast<std::uint8_t>(I2C1->DR);
                ++index;
                I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
                data[index] = static_cast<std::uint8_t>(I2C1->DR);
                ++index;
                if (!waitEvent(I2C_SR1_RXNE))
                {
                    abortTransfer();
                    return false;
                }
                data[index] = static_cast<std::uint8_t>(I2C1->DR);
            }
            return waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U);
        }
    } // namespace

    namespace
    {
        /// @brief Frees a slave left desynchronized by an MCU reset or a
        ///        debugger flash that cut a transaction short (a peripheral
        ///        SWRST cannot help there): SDA released, SCL hand-clocked
        ///        a full byte plus ACK, then a manual STOP. The clocks are
        ///        sent unconditionally: a slave can be stuck mid-byte while
        ///        letting SDA float high (its current bit is a 1), which a
        ///        STOP alone does not always clear (seen for real on the
        ///        MPU6050, deaf after a reflash until re-clocked). Pins
        ///        must still be plain GPIO.
        /// @return true when SDA ended up high (bus idle)
        bool recoverBus()
        {
            // Both pins open-drain outputs, released high; an open-drain
            // output pin still reads the line level through IDR.
            GPIOB->OTYPER = GPIOB->OTYPER | (1U << SCL_PIN) | (1U << SDA_PIN);
            GPIOB->BSRR = (1U << SCL_PIN) | (1U << SDA_PIN);
            constexpr std::uint32_t MODE_MASK = (3U << (2U * SCL_PIN)) | (3U << (2U * SDA_PIN));
            constexpr std::uint32_t MODE_OUTPUT =
                (GPIO_MODER_OUTPUT << (2U * SCL_PIN)) | (GPIO_MODER_OUTPUT << (2U * SDA_PIN));
            GPIOB->MODER = (GPIOB->MODER & ~MODE_MASK) | MODE_OUTPUT;

            for (std::uint32_t pulse = 0U; pulse < RECOVERY_CLOCKS; ++pulse)
            {
                GPIOB->BSRR = 1U << (SCL_PIN + 16U); // SCL low
                delayMs(RECOVERY_HALF_PERIOD_MS);
                GPIOB->BSRR = 1U << SCL_PIN; // SCL high
                delayMs(RECOVERY_HALF_PERIOD_MS);
            }

            // Manual STOP: SDA low then released while SCL is high, so the
            // slave sees a clean end of transaction.
            GPIOB->BSRR = 1U << (SDA_PIN + 16U);
            delayMs(RECOVERY_HALF_PERIOD_MS);
            GPIOB->BSRR = 1U << SDA_PIN;
            delayMs(RECOVERY_HALF_PERIOD_MS);

            return (GPIOB->IDR & (1U << SDA_PIN)) != 0U;
        }
    } // namespace

    bool I2cBus::init()
    {
        RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPIOBEN;
        RCC->APB1ENR = RCC->APB1ENR | RCC_APB1ENR_I2C1EN;

        if (!recoverBus())
        {
            return false;
        }

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
        I2C1->CCR = I2C_CCR_FS | I2C_CCR_400KHZ;
        I2C1->TRISE = I2C_TRISE_400KHZ;
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

    bool I2cBus::write(std::uint8_t address, const std::uint8_t *data, std::uint32_t length)
    {
        if (!m_ready || !waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U))
        {
            return false;
        }
        if (!sendStart(address, false))
        {
            abortTransfer();
            return false;
        }
        clearAddr();
        for (std::uint32_t index = 0U; index < length; ++index)
        {
            if (!waitEvent(I2C_SR1_TXE))
            {
                abortTransfer();
                return false;
            }
            I2C1->DR = data[index];
        }
        if (!waitEvent(I2C_SR1_BTF))
        {
            abortTransfer();
            return false;
        }
        I2C1->CR1 = I2C1->CR1 | I2C_CR1_STOP;
        return waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U);
    }

    bool I2cBus::read(std::uint8_t address, std::uint8_t *data, std::uint32_t length)
    {
        if (!m_ready || (length == 0U) || !waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U))
        {
            return false;
        }
        return receive(address, data, length);
    }

    bool I2cBus::writeRegister(std::uint8_t address, std::uint8_t reg, std::uint8_t value)
    {
        const std::uint8_t frame[2] = {reg, value};
        return write(address, frame, sizeof(frame));
    }

    bool I2cBus::readRegisters(std::uint8_t address,
                               std::uint8_t reg,
                               std::uint8_t *data,
                               std::uint32_t length)
    {
        if (!m_ready || (length == 0U) || !waitMasked(I2C1->SR2, I2C_SR2_BUSY, 0U))
        {
            return false;
        }
        if (!sendStart(address, false))
        {
            abortTransfer();
            return false;
        }
        clearAddr();
        if (!waitEvent(I2C_SR1_TXE))
        {
            abortTransfer();
            return false;
        }
        I2C1->DR = reg;
        if (!waitEvent(I2C_SR1_BTF))
        {
            abortTransfer();
            return false;
        }
        // Repeated start: the register pointer phase ends without a STOP.
        return receive(address, data, length);
    }
} // namespace mark4
