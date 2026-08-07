#pragma once

/// @file
/// @brief MS5611 barometer driver: command protocol, PROM calibration and
///        a non-blocking conversion state machine.

#include <cstdint>

#include "platform_stm32/i2c_bus.hpp"

namespace mark4
{
    /// MS5611 at OSR 4096 (0.012 mbar resolution). A conversion takes up
    /// to 9.04 ms, so the chip is driven by a state machine: update()
    /// advances one step per call and never blocks; pressurePa() always
    /// returns the latest completed solution. Pressure and temperature
    /// alternate, one full solution every 2 * CONVERSION_WAIT_UPDATES
    /// calls.
    class Ms5611
    {
      public:
        /// 7-bit address with the CSB pin high.
        static constexpr std::uint8_t I2C_ADDRESS = 0x77U;

        /// update() calls spent waiting for one conversion. The consumer
        /// asserts that this covers the 9.04 ms OSR-4096 conversion at its
        /// own call rate.
        static constexpr std::uint32_t CONVERSION_WAIT_UPDATES = 5U;

        /// @param bus initialized I2C bus the chip sits on
        explicit Ms5611(I2cBus &bus)
            : m_bus(bus)
        {
        }

        /// @brief Resets the chip and reads the PROM calibration, checked
        ///        against its CRC-4. Blocking (a few ms), init-time only.
        ///        Failures are logged over RTT.
        /// @return true when the calibration is loaded and consistent
        bool init();

        /// @brief Advances the conversion state machine by one step:
        ///        starts a conversion, waits, or reads the ADC. Call it
        ///        once per frame tick. An I2C failure restarts the cycle
        ///        and is counted, the previous solution stays exposed.
        void update();

        /// @return latest compensated pressure [Pa], 0 until the first
        ///         solution completes
        [[nodiscard]] float pressurePa() const
        {
            return m_pressurePa;
        }

        /// @return latest compensated temperature [degC]
        [[nodiscard]] float temperatureC() const
        {
            return m_temperatureC;
        }

        /// @return update() steps that failed on the bus
        [[nodiscard]] std::uint32_t failures() const
        {
            return m_failures;
        }

        /// @return solutions rejected by the plausibility gate (outside
        ///         [300, 1200] hPa: a broken cell, not a pressure)
        [[nodiscard]] std::uint32_t implausibleSolutions() const
        {
            return m_implausible;
        }

      private:
        /// Conversion cycle position.
        enum class State : std::uint8_t
        {
            START_PRESSURE,    ///< next step launches a D1 conversion
            WAIT_PRESSURE,     ///< D1 converting, counting calls
            START_TEMPERATURE, ///< next step reads D1 and launches D2
            WAIT_TEMPERATURE,  ///< D2 converting, counting calls
            COMPUTE,           ///< next step reads D2 and solves
        };

        /// @brief Reads the 24-bit ADC result of a finished conversion.
        /// @param[out] value receives the raw conversion
        /// @return true when the transfer completed
        bool readAdc(std::uint32_t &value);

        /// @brief Datasheet first and second order compensation, 64-bit
        ///        integer math; updates the exposed pressure/temperature.
        void solve();

        I2cBus &m_bus;                         ///< transport, not owned
        std::uint16_t m_prom[8] = {};          ///< factory word, C1..C6, CRC word
        State m_state = State::START_PRESSURE; ///< cycle position
        std::uint32_t m_waitedUpdates = 0U;    ///< calls spent in a wait state
        std::uint32_t m_rawPressure = 0U;      ///< last D1 conversion
        std::uint32_t m_rawTemperature = 0U;   ///< last D2 conversion
        float m_pressurePa = 0.0f;             ///< latest solution [Pa]
        float m_temperatureC = 0.0f;           ///< latest solution [degC]
        std::uint32_t m_failures = 0U;         ///< failed bus steps
        std::uint32_t m_implausible = 0U;      ///< solutions rejected by the gate
    };
} // namespace mark4
