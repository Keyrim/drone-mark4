#pragma once

/// @file
/// @brief USART1 transport shared by the telemetry downlink and the
///        command uplink: one full-duplex link through the FTDI dongle,
///        fully interrupt driven behind a transmit and a receive ring.

#include <cstddef>
#include <cstdint>

namespace mark4
{
    /// Fast enough for the full-rate blackbox stream plus telemetry
    /// (about 40 % of the line), and a standard FTDI rate.
    inline constexpr std::uint32_t UART1_BAUD_RATE = 921600U;

    /// @brief Configures PB6/PB7, the USART and its interrupt. Idempotent:
    ///        the sender and the receiver services both call it, the first
    ///        one wins. Assumes the 84 MHz APB2 clock of initSystemClock().
    /// @return true, kept bool for the app init contract
    bool uart1Init();

    /// @brief Queues bytes on the transmit ring, all or nothing, never
    ///        blocking on the wire.
    /// @param data bytes to send
    /// @param size byte count
    /// @return true when queued whole, false when the ring lacks room
    bool uart1TxPush(const std::uint8_t *data, std::size_t size);

    /// @brief Pops one received byte, if any.
    /// @param[out] byteOut received byte, valid only when returning true
    /// @return true when a byte was pending
    bool uart1RxPop(std::uint8_t &byteOut);
} // namespace mark4
