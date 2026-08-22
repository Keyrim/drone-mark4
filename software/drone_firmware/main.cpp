/// @file
/// @brief Firmware entry point: builds the app, runs the loop. On an init
///        failure the reason is on the RTT console and LED1 blinks fast.

#include <cstdint>

#include "firmware_app.hpp"
#include "platform_stm32/board.hpp"

namespace
{
    constexpr std::uint32_t INIT_FAILURE_BLINK_MS = 100U;
} // namespace

int main()
{
    mark4::FirmwareApp app;
    if (!app.init())
    {
        for (;;)
        {
            mark4::toggleLed1();
            mark4::delayMs(INIT_FAILURE_BLINK_MS);
        }
    }
    app.run();
}
