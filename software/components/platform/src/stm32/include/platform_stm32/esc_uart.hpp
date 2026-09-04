#pragma once

/// @file
/// @brief USART2 receive-only driver for the ESC telemetry wire: the four
///        ESCs of the 4-in-1 share one TLM line into PA3, and nothing is
///        ever sent back to them.

#include <cstdint>

namespace mark4
{
    /// @brief Configures PA3 as USART2_RX and starts the circular receive
    ///        DMA. Idempotent. Assumes the 42 MHz APB1 clock of
    ///        initSystemClock().
    /// @return true, kept bool for the app init contract
    bool escUartInit();

    /// @brief Pops one received byte, if any.
    /// @param[out] byteOut received byte, valid only when returning true
    /// @return true when a byte was pending
    bool escUartRxPop(std::uint8_t &byteOut);

    /// @return overruns seen while draining: the DMA fell behind the wire,
    ///         or the reader fell a whole lap behind the DMA
    std::uint32_t escUartRxDrops();
} // namespace mark4
