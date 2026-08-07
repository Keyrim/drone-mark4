/// @file
/// @brief Board bring-up firmware: 168 MHz clock, LED1 heartbeat, RTT
///        console and an I2C1 scan identifying the chips on the sensor
///        breakout. Grows into the flight composition root once the IMU
///        driver produces real SensorFrames.

#include <cstdint>

#include "platform_stm32/board.hpp"
#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/rtt.hpp"

namespace
{
    /// Scan bounds: 7-bit addresses outside the ranges reserved by the
    /// I2C specification.
    constexpr std::uint8_t SCAN_FIRST_ADDRESS = 0x08U;
    constexpr std::uint8_t SCAN_LAST_ADDRESS = 0x77U;

    constexpr std::uint32_t HEARTBEAT_PERIOD_MS = 500U;

    /// Heartbeat toggles between two uptime logs: one log every 10 s.
    constexpr std::uint32_t TOGGLES_PER_UPTIME_LOG = 20U;

    /// @brief Probes every valid 7-bit address and logs the responders.
    /// @param bus initialized I2C bus
    void scanI2cBus(mark4::I2cBus &bus)
    {
        mark4::rttWrite("i2c1: scanning 0x08..0x77\n");
        std::uint32_t found = 0U;
        for (std::uint8_t address = SCAN_FIRST_ADDRESS; address <= SCAN_LAST_ADDRESS; ++address)
        {
            if (bus.probe(address))
            {
                mark4::rttPrintf("i2c1: device at 0x%02X\n", static_cast<unsigned>(address));
                ++found;
            }
        }
        mark4::rttPrintf("i2c1: scan done, %u device(s)\n", static_cast<unsigned>(found));
    }
} // namespace

int main()
{
    const bool clockOk = mark4::initSystemClock();
    mark4::initCycleCounter();
    mark4::rttInit();

    mark4::rttWrite("\nmark4 bring-up\n");
    mark4::rttPrintf("clock: %lu Hz (%s)\n",
                     static_cast<unsigned long>(mark4::coreClockHz()),
                     clockOk ? "HSE + PLL" : "HSI fallback");

    mark4::initLeds();

    mark4::I2cBus bus;
    if (!clockOk)
    {
        mark4::rttWrite("i2c1: skipped, bus timing needs the full clock tree\n");
    }
    else if (bus.init())
    {
        scanI2cBus(bus);
    }
    else
    {
        mark4::rttWrite("i2c1: init failed, bus stuck busy\n");
    }

    std::uint32_t toggles = 0U;
    for (;;)
    {
        mark4::toggleLed1();
        mark4::delayMs(HEARTBEAT_PERIOD_MS);
        ++toggles;
        if ((toggles % TOGGLES_PER_UPTIME_LOG) == 0U)
        {
            const std::uint32_t seconds = (toggles * HEARTBEAT_PERIOD_MS) / 1000U;
            mark4::rttPrintf("uptime: %lu s\n", static_cast<unsigned long>(seconds));
        }
    }
}
