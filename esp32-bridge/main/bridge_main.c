/**
 * @file bridge_main.c
 * @brief Transparent UART to WiFi bridge, sitting between the flight
 *        controller and the ground tools.
 *
 * The board is a cable, not a peer: it carries bytes and never looks at them,
 * so a change of wire format needs no firmware here. It joins the network its
 * build named, or raises its own access point when there is none to join,
 * learns where to send from the first datagram the ground tool sends, then
 * forwards both directions: what the flight controller writes on the UART
 * goes out as datagrams, what comes in as datagrams goes down the UART.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

/// Log tag shared by the whole application.
static const char *TAG = "bridge";

#ifdef BRIDGE_STA_SSID
/// How long the bridge tries to join the network named at build time before
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

/// Port the bridge binds, and the one the ground tool aims at.
#define BRIDGE_UDP_PORT 47830U

/// UART wired to the flight controller, in place of the USB serial dongle.
#define BRIDGE_UART_PORT UART_NUM_1
#define BRIDGE_UART_BAUD 921600
#define BRIDGE_UART_RX_PIN 3
#define BRIDGE_UART_TX_PIN 4

/// Bytes the UART driver buffers on the downlink, about 44 ms of a full line.
/// A WiFi stall longer than that drops bytes and the frame parser on the
/// ground resynchronizes on the next sync pair.
#define BRIDGE_UART_RX_BUFFER 4096

/// Bytes the UART driver buffers on the uplink. The commands going that way
/// are rare and small; the buffer is there so writing them hands off to the
/// driver instead of holding the loop for the whole transmission.
#define BRIDGE_UART_TX_BUFFER 1024

/// Bytes gathered into one datagram, and how long the bridge waits for them.
/// Whichever comes first: the wait bounds the added latency, the size bounds
/// the datagram. What a client loses grows with the number of datagrams far
/// more than with their size: a sleeping WiFi client only gets what the access
/// point could hold for it, so one datagram per 10 ms beats five hundred per
/// second. The wait also bounds how long a command waits before reaching the
/// board, the loop having a single wait point.
#define BRIDGE_CHUNK_BYTES 1024
#define BRIDGE_CHUNK_WAIT_MS 10U

/// Bytes read from one uplink datagram. One frame of the ground tool fits
/// well inside this, and a larger datagram would be truncated rather than
/// split, which the frame parser on the board would then drop.
#define BRIDGE_UPLINK_BYTES 512

/// Address the downlink goes to, learned from the datagrams the ground tool
/// sends. Written and read by the single task of the application.
static struct sockaddr_in s_peer;

/// True once s_peer holds an address.
static bool s_peerKnown = false;

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
 * @return true when the network handed out an address, false when the bridge
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
            ESP_LOGI(TAG,
                     "joined " BRIDGE_STA_SSID " as " IPSTR ", udp port %u",
                     IP2STR(&address.ip),
                     (unsigned)BRIDGE_UDP_PORT);
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
    ESP_LOGI(TAG, "access point " BRIDGE_AP_SSID ", udp port %u", (unsigned)BRIDGE_UDP_PORT);
}

/**
 * @brief Opens the UART the flight controller is wired to.
 */
static void startUart(void)
{
    const uart_config_t config = {
        .baud_rate = BRIDGE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(
        BRIDGE_UART_PORT, BRIDGE_UART_RX_BUFFER, BRIDGE_UART_TX_BUFFER, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &config));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT,
                                 BRIDGE_UART_TX_PIN,
                                 BRIDGE_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

/**
 * @brief Opens the one socket of the bridge, bound to the fixed port.
 * @return the socket, the function never returns on failure
 */
static int openSocket(void)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "cannot open the socket: %d", errno);
        abort();
    }
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(BRIDGE_UDP_PORT);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        ESP_LOGE(TAG, "cannot bind port %u: %d", (unsigned)BRIDGE_UDP_PORT, errno);
        abort();
    }
    return sock;
}

/**
 * @brief Drains what the ground tool sent, remembers where it comes from and
 *        writes it to the flight controller. The keepalive the ground tool
 *        sends is carried like everything else: the bridge has no idea it is
 *        one, and the frame parser on the board skips it while hunting for a
 *        sync pair.
 * @param sock socket of the bridge
 */
static void pumpUplink(int sock)
{
    static uint8_t scratch[BRIDGE_UPLINK_BYTES];
    struct sockaddr_in from;
    socklen_t fromLength = sizeof(from);

    int size = 0;
    while (
        (size = recvfrom(
             sock, scratch, sizeof(scratch), MSG_DONTWAIT, (struct sockaddr *)&from, &fromLength)) >
        0)
    {
        if (!s_peerKnown || from.sin_addr.s_addr != s_peer.sin_addr.s_addr ||
            from.sin_port != s_peer.sin_port)
        {
            char text[INET_ADDRSTRLEN];
            ESP_LOGI(TAG,
                     "ground tool at %s:%u",
                     inet_ntoa_r(from.sin_addr, text, sizeof(text)),
                     (unsigned)ntohs(from.sin_port));
        }
        s_peer = from;
        s_peerKnown = true;
        fromLength = sizeof(from);

        if (uart_write_bytes(BRIDGE_UART_PORT, scratch, (size_t)size) != size)
        {
            ESP_LOGW(TAG, "dropped %d uplink bytes", size);
        }
    }
}

void app_main(void)
{
    static uint8_t chunk[BRIDGE_CHUNK_BYTES];

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
    // cost more latency than the whole aggregation window buys.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    startUart();
    const int sock = openSocket();
    ESP_LOGI(TAG, "bridge up: uart %d baud", BRIDGE_UART_BAUD);

    for (;;)
    {
        pumpUplink(sock);

        // Reading with a timeout is the whole aggregation: it returns on a
        // full chunk or when the wait expires, and it keeps draining the
        // UART even with nowhere to send, so a link that comes up late does
        // not start on a backlog of stale frames.
        const int size = uart_read_bytes(
            BRIDGE_UART_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(BRIDGE_CHUNK_WAIT_MS));
        if (size > 0 && s_peerKnown)
        {
            if (sendto(sock, chunk, (size_t)size, 0, (struct sockaddr *)&s_peer, sizeof(s_peer)) <
                0)
            {
                // The ground tool left the network. Going quiet until it says
                // hello again beats logging a line every wait period.
                ESP_LOGW(TAG, "downlink send failed (%d), waiting for a new hello", errno);
                s_peerKnown = false;
            }
        }
    }
}
