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

        /// @brief Writes raw bytes: START, address, payload, STOP. For
        ///        command-style devices without a register map.
        /// @param address 7-bit device address
        /// @param data payload bytes
        /// @param length payload size
        /// @return true when every byte was acknowledged
        bool write(std::uint8_t address, const std::uint8_t *data, std::uint32_t length);

        /// @brief Reads raw bytes: START, address in read mode, data, STOP.
        /// @param address 7-bit device address
        /// @param data receives the bytes read
        /// @param length number of bytes to read, at least 1
        /// @return true when the transfer completed
        bool read(std::uint8_t address, std::uint8_t *data, std::uint32_t length);

        /// @brief Writes one register: START, address, register, value, STOP.
        /// @param address 7-bit device address
        /// @param reg register number
        /// @param value byte written to the register
        /// @return true when the transfer completed
        bool writeRegister(std::uint8_t address, std::uint8_t reg, std::uint8_t value);

        /// @brief Reads consecutive registers with a repeated start between
        ///        the register pointer and the data phase.
        /// @param address 7-bit device address
        /// @param reg first register number
        /// @param data receives the bytes read
        /// @param length number of registers to read, at least 1
        /// @return true when the transfer completed
        bool readRegisters(std::uint8_t address,
                           std::uint8_t reg,
                           std::uint8_t *data,
                           std::uint32_t length);

      private:
        bool m_ready = false; ///< set by a successful init()
    };
} // namespace mark4
