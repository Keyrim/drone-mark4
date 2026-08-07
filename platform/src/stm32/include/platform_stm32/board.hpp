#pragma once

/// @file
/// @brief Board-level services for the mark1 flight controller: system
///        clock, busy-wait timebase and status LEDs.

#include <cstdint>

namespace mark4
{
    /// Core clock after a successful initSystemClock() [Hz].
    inline constexpr std::uint32_t CORE_CLOCK_HZ = 168000000U;

    /// Core clock at reset, and after a failed initSystemClock() [Hz].
    inline constexpr std::uint32_t HSI_CLOCK_HZ = 16000000U;

    /// @brief Switches the core to 168 MHz: 8 MHz external crystal through
    ///        the main PLL, 5 flash wait states, AHB 168 / APB1 42 /
    ///        APB2 84 MHz. On failure (crystal or PLL never ready) the chip
    ///        stays on the 16 MHz internal oscillator so the caller can
    ///        still blink and log.
    /// @return true when the core runs at CORE_CLOCK_HZ
    bool initSystemClock();

    /// @return current core clock [Hz]: CORE_CLOCK_HZ after a successful
    ///         initSystemClock(), HSI_CLOCK_HZ otherwise
    std::uint32_t coreClockHz();

    /// @brief Starts the DWT cycle counter that delayMs() runs on.
    void initCycleCounter();

    /// @brief Busy-waits for the requested duration. Requires
    ///        initCycleCounter() and a stable coreClockHz().
    /// @param milliseconds duration of the wait
    void delayMs(std::uint32_t milliseconds);

    /// @brief Configures the status LEDs (LED1 = PC13, LED2 = PC14) as
    ///        push-pull outputs.
    void initLeds();

    /// @brief Toggles LED1, the heartbeat LED.
    void toggleLed1();

    /// @brief Drives LED1, the health LED. Assumes the LED lights when the
    ///        pin is driven high.
    /// @param on true lights the LED
    void setLed1(bool on);

    /// @brief Drives LED2, the flight state LED. Same polarity as LED1.
    /// @param on true lights the LED
    void setLed2(bool on);

    /// @brief Requests an NVIC system reset and never returns: the whole
    ///        chip restarts through the reset vector, peripherals included.
    [[noreturn]] void systemReset();
} // namespace mark4
