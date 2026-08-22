#include "platform_stm32/mpu6050.hpp"

#include <cstdint>

#include "platform_stm32/board.hpp"
#include "platform_stm32/rtt.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint8_t REG_SMPLRT_DIV = 0x19U;
        constexpr std::uint8_t REG_CONFIG = 0x1AU;
        constexpr std::uint8_t REG_GYRO_CONFIG = 0x1BU;
        constexpr std::uint8_t REG_ACCEL_CONFIG = 0x1CU;
        constexpr std::uint8_t REG_INT_PIN_CFG = 0x37U;
        constexpr std::uint8_t REG_ACCEL_XOUT_H = 0x3BU;
        constexpr std::uint8_t REG_USER_CTRL = 0x6AU;
        constexpr std::uint8_t REG_PWR_MGMT_1 = 0x6BU;
        constexpr std::uint8_t REG_WHO_AM_I = 0x75U;

        constexpr std::uint8_t WHO_AM_I_VALUE = 0x68U;

        /// PWR_MGMT_1: sleep off, clock from the gyro X PLL (the datasheet
        /// recommends a gyro PLL over the internal oscillator).
        constexpr std::uint8_t PWR_CLOCK_GYRO_X_PLL = 0x01U;

        /// FS_SEL / AFS_SEL = 3: the widest range on both sensors.
        constexpr std::uint8_t FULL_SCALE_SELECT_3 = 3U << 3U;

        /// DLPF_CFG = 3: 44 Hz accel / 42 Hz gyro bandwidth, 1 kHz internal
        /// rate; SMPLRT_DIV = 0 keeps the full 1 kHz output rate.
        constexpr std::uint8_t DLPF_44HZ = 3U;
        constexpr std::uint8_t SAMPLE_RATE_DIV_NONE = 0U;

        /// USER_CTRL: the chip does not master its auxiliary bus, a
        /// prerequisite for the bypass below.
        constexpr std::uint8_t USER_CTRL_NO_AUX_MASTER = 0x00U;

        /// INT_PIN_CFG: I2C_BYPASS_EN, auxiliary bus wired to the main one.
        constexpr std::uint8_t INT_PIN_BYPASS_EN = 1U << 1U;

        constexpr std::uint32_t SAMPLE_BURST_SIZE = 14U;

        /// A transaction cut short by a reflash or reset can leave the
        /// chip's I2C engine desynchronized; the failed reads themselves
        /// resynchronize it, so the first contact gets a few attempts.
        /// Seen for real when flashing over a running firmware.
        constexpr std::uint32_t WHO_AM_I_ATTEMPTS = 5U;
        constexpr std::uint32_t WHO_AM_I_RETRY_DELAY_MS = 2U;

        /// @brief Recomposes a big-endian sensor word.
        /// @param high most significant byte
        /// @param low least significant byte
        /// @return signed 16-bit count
        std::int16_t toWord(std::uint8_t high, std::uint8_t low)
        {
            const auto word = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) |
                                                         static_cast<std::uint16_t>(low));
            return static_cast<std::int16_t>(word);
        }
    } // namespace

    bool Mpu6050::init()
    {
        std::uint8_t who = 0U;
        bool answered = false;
        for (std::uint32_t attempt = 0U; attempt < WHO_AM_I_ATTEMPTS && !answered; ++attempt)
        {
            answered = m_bus.readRegisters(I2C_ADDRESS, REG_WHO_AM_I, &who, 1U);
            if (!answered)
            {
                delayMs(WHO_AM_I_RETRY_DELAY_MS);
            }
        }
        if (!answered)
        {
            rttPrintf("mpu6050: WHO_AM_I read failed after %u attempts\n",
                      static_cast<unsigned>(WHO_AM_I_ATTEMPTS));
            return false;
        }
        if (who != WHO_AM_I_VALUE)
        {
            rttPrintf("mpu6050: unexpected WHO_AM_I 0x%02X\n", static_cast<unsigned>(who));
            return false;
        }

        const bool configured =
            m_bus.writeRegister(I2C_ADDRESS, REG_PWR_MGMT_1, PWR_CLOCK_GYRO_X_PLL) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_GYRO_CONFIG, FULL_SCALE_SELECT_3) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_ACCEL_CONFIG, FULL_SCALE_SELECT_3) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_CONFIG, DLPF_44HZ) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_SMPLRT_DIV, SAMPLE_RATE_DIV_NONE) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_USER_CTRL, USER_CTRL_NO_AUX_MASTER) &&
            m_bus.writeRegister(I2C_ADDRESS, REG_INT_PIN_CFG, INT_PIN_BYPASS_EN);
        if (!configured)
        {
            rttWrite("mpu6050: configuration write failed\n");
            return false;
        }
        return true;
    }

    bool Mpu6050::readSample(Mpu6050Sample &sample)
    {
        std::uint8_t burst[SAMPLE_BURST_SIZE];
        if (!m_bus.readRegisters(I2C_ADDRESS, REG_ACCEL_XOUT_H, burst, sizeof(burst)))
        {
            return false;
        }
        sample.accel[0] = toWord(burst[0], burst[1]);
        sample.accel[1] = toWord(burst[2], burst[3]);
        sample.accel[2] = toWord(burst[4], burst[5]);
        sample.temperature = toWord(burst[6], burst[7]);
        sample.gyro[0] = toWord(burst[8], burst[9]);
        sample.gyro[1] = toWord(burst[10], burst[11]);
        sample.gyro[2] = toWord(burst[12], burst[13]);
        return true;
    }
} // namespace mark4
