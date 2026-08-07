#pragma once

/// @file
/// @brief Blocking I2C1 master for the sensor breakout (PB8 = SCL,
///        PB9 = SDA, pull-ups on the board).

#include <cstdint>

namespace mark4
{
    /// I2C1 in standard mode (100 kHz), polling with timeouts: a stuck bus
    /// degrades into failed transfers, never into a hung loop.
    class I2cBus
    {
      public:
        /// @brief Enables the clocks, routes PB8/PB9 to the peripheral
        ///        (open-drain alternate function) and configures 100 kHz
        ///        timing. Assumes the 42 MHz APB1 clock set by
        ///        initSystemClock().
        /// @return true when the bus is idle and ready
        bool init();

        /// @brief Checks for a device: START, address in write mode, then
        ///        STOP. No data is transferred.
        /// @param address 7-bit device address
        /// @return true when the device acknowledged its address
        [[nodiscard]] bool probe(std::uint8_t address);

      private:
        bool m_ready = false; ///< set by a successful init()
    };
} // namespace mark4
