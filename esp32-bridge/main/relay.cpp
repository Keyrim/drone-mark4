/// @file
/// @brief The relay: one transport node with two links, the flight
///        controller's UART and the WiFi LAN, relaying between them. It has
///        no identity anybody learns: it never beacons, so the board's own
///        Announce is what the hub sees, at this relay's address. Towards
///        the UART only what the board needs crosses: unicasts for it, and
///        the Announce broadcasts that tell it who is on the LAN.

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "protocol/envelope.hpp"
#include "transport/node_id.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    namespace
    {
        /// Log tag of the relay.
        const char *const TAG = "relay";

        /// UART wired to the flight controller.
        constexpr uart_port_t UART_PORT = UART_NUM_1;
        constexpr int UART_BAUD = 921600;
        constexpr int UART_RX_PIN = 3;
        constexpr int UART_TX_PIN = 4;

        /// Bytes the UART driver buffers on the way in, about 44 ms of a
        /// full line: what a WiFi stall may delay the poll by before bytes
        /// are lost and the framing resynchronizes.
        constexpr int UART_RX_BUFFER = 4096;

        /// Bytes the UART driver buffers on the way out. A frame the free
        /// space cannot hold whole is refused, never waited for: the
        /// transport counts a drop and the poll loop keeps its cadence.
        constexpr int UART_TX_BUFFER = 1024;

        /// Index of the UART link in the transport: the one the filter guards.
        constexpr std::size_t UART_LINK = 0U;

        /// Poll cadence: one FreeRTOS tick, 1 ms with CONFIG_FREERTOS_HZ=1000.
        /// A 160-byte frame takes 1.7 ms on the line, so the added latency
        /// is under one frame time and every poll drains what arrived.
        constexpr TickType_t POLL_PERIOD_TICKS = 1U;

        /// Cadence of the statistics line [us].
        constexpr std::int64_t STATS_PERIOD_US = 5'000'000;

        /// Bytes of a MAC address.
        constexpr std::size_t MAC_SIZE = 6U;

        /// The UART behind the UartLink, over the ESP-IDF driver rings.
        class UartStream final : public AbsByteStream
        {
          public:
            std::size_t read(std::uint8_t *bufferOut, std::size_t capacity) override
            {
                const int got = uart_read_bytes(UART_PORT, bufferOut, capacity, 0U);
                return got > 0 ? static_cast<std::size_t>(got) : 0U;
            }

            bool write(const std::uint8_t *data, std::size_t size) override
            {
                std::size_t room = 0U;
                if (uart_get_tx_buffer_free_size(UART_PORT, &room) != ESP_OK || room < size)
                {
                    ++m_txFull;
                    return false;
                }
                return uart_write_bytes(UART_PORT, data, size) == static_cast<int>(size);
            }

            /// @return frames refused because the transmit ring was full
            [[nodiscard]] std::uint32_t txFull() const
            {
                return m_txFull;
            }

          private:
            std::uint32_t m_txFull = 0U; ///< frames refused for lack of room
        };

        /// The one rule of the relay: a broadcast only goes down the UART
        /// when it is an Announce (the board learns the LAN nodes from
        /// those); a unicast routed there is for the board by construction.
        /// Everything else the LAN broadcasts (drone_sim telemetry, run
        /// stats, logs) stays on the LAN.
        bool uartFilter(void *context,
                        std::size_t linkIndex,
                        const FrameHeader &header,
                        const std::uint8_t *payload,
                        std::size_t size)
        {
            static_cast<void>(context);
            return linkIndex != UART_LINK || header.dst != BROADCAST_NODE ||
                   envelopeIsAnnounce(payload, size);
        }

        /// @brief Logs a node the transport just heard for the first time.
        void onNodeUp(void *context, const Transport::Node &node)
        {
            static_cast<void>(context);
            if (node.link == UART_LINK)
            {
                ESP_LOGI(TAG, "node %08" PRIx32 " up on the uart", node.id);
                return;
            }
            ESP_LOGI(TAG,
                     "node %08" PRIx32 " up on the lan at %u.%u.%u.%u:%u",
                     node.id,
                     static_cast<unsigned>(node.address.host >> 24U),
                     static_cast<unsigned>((node.address.host >> 16U) & 0xFFU),
                     static_cast<unsigned>((node.address.host >> 8U) & 0xFFU),
                     static_cast<unsigned>(node.address.host & 0xFFU),
                     static_cast<unsigned>(node.address.port));
        }

        /// @brief Logs a node the transport just forgot.
        void onNodeDown(void *context, const Transport::Node &node)
        {
            static_cast<void>(context);
            ESP_LOGI(TAG, "node %08" PRIx32 " gone", node.id);
        }

        /// @brief Opens the UART the flight controller is wired to.
        void startUart()
        {
            uart_config_t config = {};
            config.baud_rate = UART_BAUD;
            config.data_bits = UART_DATA_8_BITS;
            config.parity = UART_PARITY_DISABLE;
            config.stop_bits = UART_STOP_BITS_1;
            config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
            config.source_clk = UART_SCLK_DEFAULT;
            ESP_ERROR_CHECK(
                uart_driver_install(UART_PORT, UART_RX_BUFFER, UART_TX_BUFFER, 0, nullptr, 0));
            ESP_ERROR_CHECK(uart_param_config(UART_PORT, &config));
            ESP_ERROR_CHECK(uart_set_pin(
                UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        }

        /// @return this station's or access point's IPv4 address, host byte
        ///         order, 0 when the network interface has none
        std::uint32_t ownAddress()
        {
            esp_netif_t *const netif = esp_netif_get_default_netif();
            esp_netif_ip_info_t info = {};
            if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK)
            {
                return 0U;
            }
            return ntohl(info.ip.addr);
        }

        /// The composition: services as members, declaration order is
        /// construction order, dependencies by reference.
        struct Relay
        {
            UartStream stream;     ///< the UART driver rings
            UartLink uart{stream}; ///< the board's link, serial framing
            UdpLink lan;           ///< the WiFi LAN, discovery port 47820
            Transport transport;   ///< the node, relaying between the two

            /// @param nodeId this relay's transport identity
            explicit Relay(std::uint32_t nodeId)
                : transport(nodeId)
            {
            }
        };
    } // namespace
} // namespace mark4

/// @brief Runs the relay forever, once the network is up. Called from
///        app_main() after the WiFi bring-up.
extern "C" void relayRun(void)
{
    using namespace mark4;

    std::array<std::uint8_t, MAC_SIZE> mac{};
    ESP_ERROR_CHECK(esp_read_mac(mac.data(), ESP_MAC_WIFI_STA));
    // Static: the composition lives for the whole run and is too large for
    // the main task's stack (two frame buffers and the node table).
    static Relay relay(hashNodeId(mac.data(), mac.size()));

    startUart();
    if (!relay.lan.init() || !relay.transport.addLink(relay.uart) ||
        !relay.transport.addLink(relay.lan) || !relay.transport.init())
    {
        ESP_LOGE(TAG, "cannot start the transport");
        std::abort();
    }
    // lwIP has no getifaddrs(): the LAN link is told the one address a
    // broadcast of ours can come back from, so the echo is dropped instead
    // of counted as a duplicate of every frame relayed.
    relay.lan.addLocalHost(ownAddress());
    relay.transport.setRelay(true);
    relay.transport.setRelayFilter(&uartFilter, nullptr);
    relay.transport.setNodeCallbacks(&onNodeUp, &onNodeDown, nullptr);
    // No setBeacon(): the relay announces nothing, the board does.
    ESP_LOGI(TAG,
             "relay up: node %08" PRIx32 ", uart %d baud, lan data port %u, discovery port %u",
             relay.transport.nodeId(),
             UART_BAUD,
             static_cast<unsigned>(relay.lan.dataPort()),
             static_cast<unsigned>(relay.lan.discoveryPort()));

    std::int64_t nextStatsUs = esp_timer_get_time() + STATS_PERIOD_US;
    for (;;)
    {
        // Nothing is for this node: every payload is relayed or dropped, so
        // no deliver callback. The poll drains both links whole.
        relay.transport.poll(static_cast<std::uint64_t>(esp_timer_get_time()), nullptr, nullptr);
        const std::int64_t nowUs = esp_timer_get_time();
        if (nowUs >= nextStatsUs)
        {
            nextStatsUs = nowUs + STATS_PERIOD_US;
            ESP_LOGI(TAG,
                     "nodes %u, relayed %" PRIu32 ", filtered %" PRIu32 ", dropped %" PRIu32
                     ", uart tx full %" PRIu32,
                     static_cast<unsigned>(relay.transport.nodeCount()),
                     relay.transport.relayed(),
                     relay.transport.filtered(),
                     relay.transport.dropped(),
                     relay.stream.txFull());
        }
        vTaskDelay(POLL_PERIOD_TICKS);
    }
}
