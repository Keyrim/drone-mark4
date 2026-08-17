/**
 * @file bridge_main.c
 * @brief Heartbeat firmware for the ESP32 bridge board.
 *
 * Proves the toolchain, the flash image and the console: one log line per
 * second and nothing else. No WiFi, no UART link, no GPIO, so the same binary
 * runs on any ESP32-C3 module whatever it wires to its pins.
 */

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/// Log tag shared by the whole application.
static const char *TAG = "bridge";

/// Period of the heartbeat line, in milliseconds.
#define HEARTBEAT_PERIOD_MS 1000U

void app_main(void)
{
    uint32_t beat = 0U;

    ESP_LOGI(TAG, "esp32-bridge starting");

    for (;;)
    {
        ESP_LOGI(TAG, "alive, beat %lu", (unsigned long)beat);
        ++beat;
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}
