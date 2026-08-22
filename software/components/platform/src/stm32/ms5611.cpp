#include "platform_stm32/ms5611.hpp"

#include <cstdint>

#include "platform_stm32/board.hpp"
#include "platform_stm32/rtt.hpp"

namespace mark4
{
    namespace
    {
        constexpr std::uint8_t CMD_RESET = 0x1EU;
        constexpr std::uint8_t CMD_CONVERT_D1_OSR4096 = 0x48U;
        constexpr std::uint8_t CMD_CONVERT_D2_OSR4096 = 0x58U;
        constexpr std::uint8_t CMD_READ_ADC = 0x00U;
        constexpr std::uint8_t CMD_READ_PROM_BASE = 0xA0U; ///< + 2 * word index

        constexpr std::uint32_t RESET_DELAY_MS = 3U;
        constexpr std::uint32_t PROM_WORDS = 8U;

        /// @brief Datasheet (AN520) CRC-4 over the PROM, its own CRC
        ///        nibble zeroed out.
        /// @param prom the 8 PROM words
        /// @return true when the computed CRC matches the stored nibble
        bool promCrcOk(const std::uint16_t prom[PROM_WORDS])
        {
            const std::uint16_t storedCrc = prom[7] & 0x000FU;
            std::uint16_t remainder = 0U;
            for (std::uint32_t byteIndex = 0U; byteIndex < 2U * PROM_WORDS; ++byteIndex)
            {
                std::uint16_t word = prom[byteIndex / 2U];
                if (byteIndex == (2U * PROM_WORDS) - 1U)
                {
                    word = word & 0xFF00U; // the CRC nibble is not part of itself
                }
                if ((byteIndex % 2U) == 0U)
                {
                    remainder = remainder ^ static_cast<std::uint16_t>(word >> 8U);
                }
                else
                {
                    remainder = remainder ^ static_cast<std::uint16_t>(word & 0x00FFU);
                }
                for (std::uint32_t bit = 0U; bit < 8U; ++bit)
                {
                    if ((remainder & 0x8000U) != 0U)
                    {
                        remainder = static_cast<std::uint16_t>(remainder << 1U) ^ 0x3000U;
                    }
                    else
                    {
                        remainder = static_cast<std::uint16_t>(remainder << 1U);
                    }
                }
            }
            return ((remainder >> 12U) & 0x000FU) == storedCrc;
        }
    } // namespace

    bool Ms5611::init()
    {
        if (!m_bus.write(I2C_ADDRESS, &CMD_RESET, 1U))
        {
            rttWrite("ms5611: reset failed\n");
            return false;
        }
        delayMs(RESET_DELAY_MS);

        for (std::uint32_t word = 0U; word < PROM_WORDS; ++word)
        {
            const auto command = static_cast<std::uint8_t>(CMD_READ_PROM_BASE + (2U * word));
            std::uint8_t raw[2] = {0U, 0U};
            if (!m_bus.write(I2C_ADDRESS, &command, 1U) ||
                !m_bus.read(I2C_ADDRESS, raw, sizeof(raw)))
            {
                rttPrintf("ms5611: PROM word %u read failed\n", static_cast<unsigned>(word));
                return false;
            }
            m_prom[word] =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[0]) << 8U) | raw[1]);
        }
        if (!promCrcOk(m_prom))
        {
            rttWrite("ms5611: PROM CRC mismatch\n");
            return false;
        }

        return true;
    }

    bool Ms5611::readAdc(std::uint32_t &value)
    {
        // Repeated start between the 0x00 command and the data, exactly
        // the datasheet sequence: a STOP in between shifts the result.
        std::uint8_t raw[3] = {0U, 0U, 0U};
        if (!m_bus.readRegisters(I2C_ADDRESS, CMD_READ_ADC, raw, sizeof(raw)))
        {
            return false;
        }
        value = (static_cast<std::uint32_t>(raw[0]) << 16U) |
                (static_cast<std::uint32_t>(raw[1]) << 8U) | raw[2];
        return true;
    }

    void Ms5611::update()
    {
        switch (m_state)
        {
            case State::START_PRESSURE:
                if (m_bus.write(I2C_ADDRESS, &CMD_CONVERT_D1_OSR4096, 1U))
                {
                    m_waitedUpdates = 0U;
                    m_state = State::WAIT_PRESSURE;
                }
                else
                {
                    ++m_failures;
                }
                break;

            case State::WAIT_PRESSURE:
                ++m_waitedUpdates;
                if (m_waitedUpdates >= CONVERSION_WAIT_UPDATES)
                {
                    m_state = State::START_TEMPERATURE;
                }
                break;

            case State::START_TEMPERATURE:
                if (readAdc(m_rawPressure) && m_bus.write(I2C_ADDRESS, &CMD_CONVERT_D2_OSR4096, 1U))
                {
                    m_waitedUpdates = 0U;
                    m_state = State::WAIT_TEMPERATURE;
                }
                else
                {
                    ++m_failures;
                    m_state = State::START_PRESSURE;
                }
                break;

            case State::WAIT_TEMPERATURE:
                ++m_waitedUpdates;
                if (m_waitedUpdates >= CONVERSION_WAIT_UPDATES)
                {
                    m_state = State::COMPUTE;
                }
                break;

            case State::COMPUTE:
                if (readAdc(m_rawTemperature))
                {
                    solve();
                }
                else
                {
                    ++m_failures;
                }
                m_state = State::START_PRESSURE;
                break;
        }
    }

    void Ms5611::solve()
    {
        // Datasheet naming: C1..C6 are m_prom[1..6], D1/D2 the raw
        // conversions; TEMP in 0.01 degC, OFF/SENS in datasheet LSB, P in
        // 0.01 mbar which lands exactly on pascals.
        const auto d1 = static_cast<std::int64_t>(m_rawPressure);
        const auto d2 = static_cast<std::int64_t>(m_rawTemperature);
        const std::int64_t dT = d2 - (static_cast<std::int64_t>(m_prom[5]) << 8U);
        std::int64_t temp = 2000 + ((dT * static_cast<std::int64_t>(m_prom[6])) >> 23U);
        std::int64_t off = (static_cast<std::int64_t>(m_prom[2]) << 16U) +
                           ((static_cast<std::int64_t>(m_prom[4]) * dT) >> 7U);
        std::int64_t sens = (static_cast<std::int64_t>(m_prom[1]) << 15U) +
                            ((static_cast<std::int64_t>(m_prom[3]) * dT) >> 8U);

        // Second order compensation below 20 degC (and further below
        // -15 degC), straight from the datasheet.
        if (temp < 2000)
        {
            const std::int64_t t2 = (dT * dT) >> 31U;
            const std::int64_t deviation = temp - 2000;
            std::int64_t off2 = (5 * deviation * deviation) / 2;
            std::int64_t sens2 = (5 * deviation * deviation) / 4;
            if (temp < -1500)
            {
                const std::int64_t cold = temp + 1500;
                off2 += 7 * cold * cold;
                sens2 += (11 * cold * cold) / 2;
            }
            temp -= t2;
            off -= off2;
            sens -= sens2;
        }

        const std::int64_t pressure = (((d1 * sens) >> 21U) - off) >> 15U;

        // Plausibility gate: outside [300, 1200] hPa the solution is not
        // a pressure (broken or counterfeit cell, values near ADC full
        // scale); it must not feed the altitude estimation.
        if (pressure < 30000 || pressure > 120000)
        {
            if (m_implausible == 0U)
            {
                rttPrintf("ms5611: implausible solution %ld Pa, sensor faulty\n",
                          static_cast<long>(pressure));
            }
            ++m_implausible;
            return;
        }
        m_pressurePa = static_cast<float>(pressure);
        m_temperatureC = static_cast<float>(temp) * 0.01f;
    }
} // namespace mark4
