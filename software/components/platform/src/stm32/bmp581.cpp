#include "platform_stm32/bmp581.hpp"

#include <cstdint>

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "platform_stm32/board.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_PLATFORM_BARO, "platform/baro"};
    } // namespace

    namespace
    {
        constexpr std::uint8_t REG_CHIP_ID = 0x01U;
        constexpr std::uint8_t REG_TEMP_DATA_XLSB = 0x1DU; ///< first of the 6 data bytes
        constexpr std::uint8_t REG_INT_STATUS = 0x27U;
        constexpr std::uint8_t REG_STATUS = 0x28U;
        constexpr std::uint8_t REG_DSP_CONFIG = 0x30U;
        constexpr std::uint8_t REG_DSP_IIR = 0x31U;
        constexpr std::uint8_t REG_OSR_CONFIG = 0x36U;
        constexpr std::uint8_t REG_ODR_CONFIG = 0x37U;
        constexpr std::uint8_t REG_OSR_EFF = 0x38U;
        constexpr std::uint8_t REG_CMD = 0x7EU;

        /// Two silicon revisions ship under the same part number.
        constexpr std::uint8_t CHIP_ID_PRIMARY = 0x50U;
        constexpr std::uint8_t CHIP_ID_SECONDARY = 0x51U;

        constexpr std::uint8_t CMD_SOFT_RESET = 0xB6U;

        constexpr std::uint8_t INT_STATUS_POR = 1U << 4U; ///< reset completed
        constexpr std::uint8_t STATUS_NVM_RDY = 1U << 1U; ///< trimming loaded
        constexpr std::uint8_t STATUS_NVM_ERR = 1U << 2U; ///< trimming unusable
        constexpr std::uint8_t OSR_EFF_ODR_VALID = 1U << 7U;

        /// The SDO strap decides which one answers; probing both makes the
        /// wiring irrelevant to this driver.
        constexpr std::uint8_t ADDRESSES[2] = {Bmp581::I2C_ADDRESS_SDO_LOW,
                                               Bmp581::I2C_ADDRESS_SDO_HIGH};

        /// OSR_CONFIG: press_en (bit 6), osr_p (bits 5:3) = x16, osr_t
        /// (bits 2:0) = x1. Pressure at x16 is the datasheet "high
        /// resolution" point, 0.21 Pa RMS (about 2 cm of altitude), and
        /// temperature only trims the pressure here so x1 is plenty.
        constexpr std::uint8_t OSR_CONFIG_VALUE = (1U << 6U) | (4U << 3U) | 0U;

        /// ODR_CONFIG.odr = 0x0C, 80 Hz: the maximum the datasheet allows
        /// for osr_p x16 / osr_t x1 in NORMAL mode.
        constexpr std::uint8_t ODR_80HZ = 0x0CU;

        /// ODR_CONFIG with deep standby disabled (bit 7): the low-power
        /// duty cycling would add milliseconds of latency to every sample.
        constexpr std::uint8_t ODR_CONFIG_STANDBY = (1U << 7U) | (ODR_80HZ << 2U);
        constexpr std::uint8_t ODR_CONFIG_NORMAL = ODR_CONFIG_STANDBY | 0x01U;

        /// DSP_IIR: set_iir_p (bits 5:3) = coefficient 3, set_iir_t
        /// (bits 2:0) = bypass. At 80 Hz a coefficient of 3 cuts the
        /// sensor and wind noise around 4 Hz while staying far above the
        /// vertical dynamics of a throw; a stronger filter would delay the
        /// apex more than it would clean the signal.
        constexpr std::uint8_t DSP_IIR_VALUE = 2U << 3U;

        /// DSP_CONFIG: shdw_sel_iir_p (bit 5) so the data registers hand
        /// out the filtered pressure rather than the raw one. Bits 1:0 are
        /// reserved and written back at their 0b11 reset value.
        constexpr std::uint8_t DSP_CONFIG_VALUE = 0x03U | (1U << 5U);

        constexpr std::uint32_t DATA_BURST_SIZE = 6U;

        /// Datasheet timings, rounded up: tsoft_res 2 ms, tstandby 2.5 ms,
        /// and t_startup 3 ms from standby (deep_dis is set, so the 4 ms
        /// tstartup_deep never applies).
        constexpr std::uint32_t SOFT_RESET_DELAY_MS = 3U;
        constexpr std::uint32_t STANDBY_DELAY_MS = 3U;
        constexpr std::uint32_t STARTUP_DELAY_MS = 5U;

        /// Same reason as the IMU: a transaction cut short by a reflash
        /// leaves the chip's I2C engine desynchronized, and the failed
        /// reads themselves resynchronize it.
        constexpr std::uint32_t PROBE_ATTEMPTS = 5U;
        constexpr std::uint32_t PROBE_RETRY_DELAY_MS = 2U;

        /// Output scales: pressure is (unsigned, 24, 6) [Pa], temperature
        /// (signed, 24, 16) [degC], both little-endian across the three
        /// data registers.
        constexpr float PA_PER_LSB = 1.0f / 64.0f;
        constexpr float DEGC_PER_LSB = 1.0f / 65536.0f;

        /// The six data registers reset to 0x7F each, so this word is what
        /// a burst reads before the first measurement lands.
        constexpr std::uint32_t DATA_RESET_PATTERN = 0x7F7F7FU;

        constexpr float PRESSURE_MIN_PA = 30000.0f;
        constexpr float PRESSURE_MAX_PA = 120000.0f;

        /// @brief Recomposes a little-endian 24-bit output word.
        /// @param xlsb least significant byte
        /// @param lsb middle byte
        /// @param msb most significant byte
        /// @return the 24-bit value, zero extended
        std::uint32_t toWord24(std::uint8_t xlsb, std::uint8_t lsb, std::uint8_t msb)
        {
            return (static_cast<std::uint32_t>(msb) << 16U) |
                   (static_cast<std::uint32_t>(lsb) << 8U) | static_cast<std::uint32_t>(xlsb);
        }
    } // namespace

    bool Bmp581::init()
    {
        for (std::uint32_t attempt = 0U; attempt < PROBE_ATTEMPTS && m_address == 0U; ++attempt)
        {
            for (const std::uint8_t address : ADDRESSES)
            {
                std::uint8_t id = 0U;
                if (!m_bus.readRegisters(address, REG_CHIP_ID, &id, 1U))
                {
                    continue; // nothing acknowledged there, try the other strap
                }
                if (id == CHIP_ID_PRIMARY || id == CHIP_ID_SECONDARY)
                {
                    m_address = address;
                    break;
                }
                MODULE.error("0x%02X answered with chip id 0x%02X, not a BMP581",
                             static_cast<unsigned>(address),
                             static_cast<unsigned>(id));
            }
            if (m_address == 0U)
            {
                delayMs(PROBE_RETRY_DELAY_MS);
            }
        }
        if (m_address == 0U)
        {
            MODULE.error("no chip id at 0x%02X or 0x%02X after %u attempts",
                         static_cast<unsigned>(I2C_ADDRESS_SDO_LOW),
                         static_cast<unsigned>(I2C_ADDRESS_SDO_HIGH),
                         static_cast<unsigned>(PROBE_ATTEMPTS));
            return false;
        }
        MODULE.info("found at 0x%02X", static_cast<unsigned>(m_address));

        if (!m_bus.writeRegister(m_address, REG_CMD, CMD_SOFT_RESET))
        {
            MODULE.error("soft reset failed");
            return false;
        }
        delayMs(SOFT_RESET_DELAY_MS);

        // Post-reset checks the datasheet asks for, in its own order:
        // the power-on flag (clear-on-read, so read it once) then the NVM
        // status that says the factory trimming actually loaded.
        std::uint8_t intStatus = 0U;
        std::uint8_t status = 0U;
        if (!m_bus.readRegisters(m_address, REG_INT_STATUS, &intStatus, 1U) ||
            !m_bus.readRegisters(m_address, REG_STATUS, &status, 1U))
        {
            MODULE.error("status read failed");
            return false;
        }
        if ((intStatus & INT_STATUS_POR) == 0U)
        {
            MODULE.error("reset never completed, INT_STATUS 0x%02X",
                         static_cast<unsigned>(intStatus));
            return false;
        }
        if ((status & STATUS_NVM_RDY) == 0U || (status & STATUS_NVM_ERR) != 0U)
        {
            MODULE.error("NVM not ready, STATUS 0x%02X", static_cast<unsigned>(status));
            return false;
        }

        // The chip wakes in deep standby, where the configuration
        // registers below are read-only; the first write leaves it.
        if (!m_bus.writeRegister(m_address, REG_ODR_CONFIG, ODR_CONFIG_STANDBY))
        {
            MODULE.error("standby entry failed");
            return false;
        }
        delayMs(STANDBY_DELAY_MS);

        const bool configured = m_bus.writeRegister(m_address, REG_DSP_IIR, DSP_IIR_VALUE) &&
                                m_bus.writeRegister(m_address, REG_DSP_CONFIG, DSP_CONFIG_VALUE) &&
                                m_bus.writeRegister(m_address, REG_OSR_CONFIG, OSR_CONFIG_VALUE) &&
                                m_bus.writeRegister(m_address, REG_ODR_CONFIG, ODR_CONFIG_NORMAL);
        if (!configured)
        {
            MODULE.error("configuration write failed");
            return false;
        }
        delayMs(STARTUP_DELAY_MS);

        // The chip validates the rate against the oversampling itself, and
        // this configuration sits on the last valid row of the datasheet
        // table with a rate specified to a few percent. A rejected pair is
        // not a dead sensor though: the chip keeps the requested rate and
        // falls back to x2 oversampling, which is still a usable barometer
        // (0.58 Pa RMS), so this is reported and not treated as a failure.
        std::uint8_t effective = 0U;
        if (!m_bus.readRegisters(m_address, REG_OSR_EFF, &effective, 1U))
        {
            MODULE.error("effective rate read failed");
            return false;
        }
        if ((effective & OSR_EFF_ODR_VALID) == 0U)
        {
            MODULE.warn("rate/oversampling pair refused, running the chip's fallback "
                        "oversampling (OSR_EFF 0x%02X)",
                        static_cast<unsigned>(effective));
        }
        m_ready = true;
        return true;
    }

    void Bmp581::update()
    {
        if (!m_ready)
        {
            return; // never came up: no bus traffic, no pressure, no counters
        }
        ++m_ticks;
        if (m_ticks < TICKS_PER_READ)
        {
            return;
        }
        m_ticks = 0U;

        std::uint8_t burst[DATA_BURST_SIZE];
        if (!m_bus.readRegisters(m_address, REG_TEMP_DATA_XLSB, burst, sizeof(burst)))
        {
            ++m_failures;
            return;
        }

        // No data-ready poll: the data registers always hold the last
        // completed measurement, so a read landing between two outputs
        // simply repeats the previous one instead of costing a transfer.
        const std::uint32_t rawTemperature = toWord24(burst[0], burst[1], burst[2]);
        const std::uint32_t rawPressure = toWord24(burst[3], burst[4], burst[5]);
        if (rawPressure == DATA_RESET_PATTERN)
        {
            return; // the first measurement has not landed yet, not a fault
        }
        const float pressurePa = static_cast<float>(rawPressure) * PA_PER_LSB;

        // Plausibility gate: outside [300, 1200] hPa the solution is not a
        // pressure (broken cell, or an output frozen near full scale); it
        // must not feed the altitude estimation.
        if (pressurePa < PRESSURE_MIN_PA || pressurePa > PRESSURE_MAX_PA)
        {
            if (m_implausible == 0U)
            {
                MODULE.warn("implausible solution %ld Pa, sensor faulty",
                            static_cast<long>(pressurePa));
            }
            ++m_implausible;
            return;
        }
        m_pressurePa = pressurePa;
        // Temperature is signed over 24 bits: sign extend before scaling.
        // The datasheet only calls the format signed, it never spells out
        // two's complement; this matches what the Bosch BMP5-SensorAPI
        // does with the same three bytes.
        m_temperatureC =
            static_cast<float>(static_cast<std::int32_t>(rawTemperature << 8U) >> 8) * DEGC_PER_LSB;
    }
} // namespace mark4
