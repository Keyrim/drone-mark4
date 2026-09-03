#pragma once

/// @file
/// @brief BMP581 barometer driver over I2C: address probe, configuration
///        and one burst read of the already compensated output.

#include <cstdint>

#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/sensor_health.hpp"
#include "telemetry/registry.hpp"

namespace mark4
{
    /// BMP581 left running in NORMAL mode: the chip samples, compensates
    /// and IIR-filters on its own, so there is no calibration PROM and no
    /// conversion state machine here. update() is a single 6-byte burst
    /// and pressurePa() returns the latest sample the chip published.
    class Bmp581
    {
      public:
        /// 7-bit address with the SDO pin low; init() probes both so the
        /// breakout can be strapped either way.
        static constexpr std::uint8_t I2C_ADDRESS_SDO_LOW = 0x46U;

        /// 7-bit address with the SDO pin high.
        static constexpr std::uint8_t I2C_ADDRESS_SDO_HIGH = 0x47U;

        /// Output data rate the chip is configured for [Hz].
        static constexpr std::uint32_t OUTPUT_RATE_HZ = 80U;

        /// update() calls between two bus reads. The chip publishes at
        /// OUTPUT_RATE_HZ, so reading on every tick of a much faster loop
        /// would spend most transfers re-reading the same sample and would
        /// expose the loop to one I2C timeout per tick for nothing. The
        /// consumer asserts that its own rate divided by this still leaves
        /// margin over OUTPUT_RATE_HZ.
        static constexpr std::uint32_t TICKS_PER_READ = 3U;

        /// Oldest age a solution may have for a frame to carry it as valid
        /// [us]. The chip publishes every 12.5 ms and is read every
        /// TICKS_PER_READ ticks, so a working baro is always well inside;
        /// past it the frame says "no baro" rather than repeating a stale
        /// pressure.
        static constexpr std::uint64_t FRESH_MAX_AGE_US = 50000U;

        /// Failure duration after which the WARN of the first failed read
        /// becomes an ERROR [us].
        static constexpr std::uint64_t FAULT_HORIZON_US = 500000U;

        /// @param bus initialized I2C bus the chip sits on
        explicit Bmp581(I2cBus &bus);

        /// @brief Probes both addresses for the chip id, soft-resets the
        ///        chip, checks the power-on and NVM status, writes the
        ///        oversampling / rate / filter configuration and enters
        ///        NORMAL mode. Blocking (about 10 ms), init-time only.
        ///        Failures are logged over RTT.
        /// @return true when the chip answered and took the configuration
        bool init();

        /// @brief Reads the latest temperature and pressure output, once
        ///        every TICKS_PER_READ calls. Call it once per frame tick.
        ///        An I2C failure or an implausible solution is counted, fed
        ///        to the health tracker (which logs the transitions) and
        ///        leaves the previous solution exposed; fresh() says
        ///        whether that solution is recent enough to trust. Inert
        ///        until a successful init(), so a chip that never came up
        ///        costs nothing per frame and simply reports no pressure.
        /// @param nowUs instant of the call [us]
        void update(std::uint64_t nowUs);

        /// @param nowUs current instant [us]
        /// @return true when a plausible solution was read no more than
        ///         FRESH_MAX_AGE_US ago
        [[nodiscard]] bool fresh(std::uint64_t nowUs) const
        {
            return m_health.freshWithin(nowUs, FRESH_MAX_AGE_US);
        }

        /// @return latest compensated pressure [Pa], 0 until the first
        ///         solution is read
        [[nodiscard]] float pressurePa() const
        {
            return m_pressurePa;
        }

        /// @return latest compensated temperature [degC]
        [[nodiscard]] float temperatureC() const
        {
            return m_temperatureC;
        }

        /// @return update() reads that failed on the bus
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
        I2cBus &m_bus;                    ///< transport, not owned
        std::uint8_t m_address = 0U;      ///< address the probe locked onto
        bool m_ready = false;             ///< set by a successful init()
        std::uint32_t m_ticks = 0U;       ///< calls since the last bus read
        float m_pressurePa = 0.0f;        ///< latest solution [Pa]
        float m_temperatureC = 0.0f;      ///< latest solution [degC]
        std::uint32_t m_failures = 0U;    ///< failed bus reads
        std::uint32_t m_implausible = 0U; ///< solutions rejected by the gate
        SensorHealth m_health;            ///< read outcomes and their logs

        /// The chip compensates its pressure with this, so it is the one
        /// temperature the board already measures: worth watching next to
        /// the altitude channel it feeds.
        TelemetryEntry m_temperatureEntry{
            "sensor/baro_temperature", TelemetryUnit::CELSIUS, m_temperatureC};
    };
} // namespace mark4
