/**
 * @file bridge_main.c
 * @brief Network bring-up of the ESP32 relay, then hand-over to the relay
 *        itself (relay.cpp).
 *
 * The board joins the network its build named, or raises its own access
 * point when there is none to join, then runs the transport relay between
 * the flight controller's UART and the WiFi LAN for the rest of its life.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/// Log tag of the bring-up.
static const char *TAG = "bridge";

#ifdef BRIDGE_STA_SSID
/// How long the board tries to join the network named at build time before
/// giving up and raising its own access point. A router that answers takes a
/// second or two; anything longer means it is not there.
#define BRIDGE_STA_TIMEOUT_MS 10000
/// Delay between two looks at whether the network handed out an address.
#define BRIDGE_STA_POLL_MS 200
#endif

/// Access point the ground tool joins. The address is the ESP-IDF default for
/// an access point, 192.168.4.1, and the clients get theirs by DHCP.
#define BRIDGE_AP_SSID "mark4-bridge"
#define BRIDGE_AP_PASSWORD "mark4mark4"
#define BRIDGE_AP_CHANNEL 1U
#define BRIDGE_AP_MAX_CLIENTS 2U

/// The relay proper, relay.cpp: never returns.
void relayRun(void);

/**
 * @brief Brings up the parts both modes need.
 */
static void startWifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
}

#ifdef BRIDGE_STA_SSID
/**
 * @brief Keeps asking to join, the station stopping on its own otherwise.
 */
static void onStationDisconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    (void)esp_wifi_connect();
}

/**
 * @brief Joins the network named at build time.
 * @return true when the network handed out an address, false when the board
 *         should raise its own access point instead
 */
static bool joinNetwork(void)
{
    esp_netif_t *const netif = esp_netif_create_default_wifi_sta();

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, BRIDGE_STA_SSID, sizeof(BRIDGE_STA_SSID));
    memcpy(config.sta.password, BRIDGE_STA_PASSWORD, sizeof(BRIDGE_STA_PASSWORD));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStationDisconnected, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    for (int waited = 0; waited < BRIDGE_STA_TIMEOUT_MS; waited += BRIDGE_STA_POLL_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_STA_POLL_MS));
        esp_netif_ip_info_t address;
        if (esp_netif_get_ip_info(netif, &address) == ESP_OK && address.ip.addr != 0U)
        {
            ESP_LOGI(TAG, "joined " BRIDGE_STA_SSID " as " IPSTR, IP2STR(&address.ip));
            return true;
        }
    }

    ESP_LOGW(TAG, BRIDGE_STA_SSID " did not answer, falling back to the access point");
    ESP_ERROR_CHECK(esp_event_handler_unregister(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onStationDisconnected));
    ESP_ERROR_CHECK(esp_wifi_stop());
    esp_netif_destroy_default_wifi(netif);
    return false;
}
#endif

/**
 * @brief Raises the access point.
 */
static void startAccessPoint(void)
{
    (void)esp_netif_create_default_wifi_ap();

    wifi_config_t config = {0};
    memcpy(config.ap.ssid, BRIDGE_AP_SSID, sizeof(BRIDGE_AP_SSID));
    config.ap.ssid_len = (uint8_t)(sizeof(BRIDGE_AP_SSID) - 1U);
    memcpy(config.ap.password, BRIDGE_AP_PASSWORD, sizeof(BRIDGE_AP_PASSWORD));
    config.ap.channel = BRIDGE_AP_CHANNEL;
    config.ap.max_connection = BRIDGE_AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "access point " BRIDGE_AP_SSID);
}

void app_main(void)
{
    esp_err_t status = nvs_flash_init();
    if (status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        status = nvs_flash_init();
    }
    ESP_ERROR_CHECK(status);

    startWifi();
#ifdef BRIDGE_STA_SSID
    if (!joinNetwork())
#endif
    {
        startAccessPoint();
    }
    // The link carries a continuous stream: sleeping between beacons would
    // cost more latency than anything it saves.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    relayRun();
}
