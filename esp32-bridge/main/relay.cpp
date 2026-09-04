/// @file
/// @brief The relay: one transport node with two links, the flight
///        controller's UART and the WiFi LAN, relaying between them. It is a
///        node of its own too: it announces itself as a relay on both links,
///        logs through the log library over the LAN, and answers the
///        LogControl addressed to it, and it updates itself over the air:
///        the same OtaUpdater the flight controller runs, over a store that
///        translates to the ESP-IDF OTA partitions, fed by the Ota*
///        unicasts a hub sends it. Towards the UART only what the board
///        needs crosses: unicasts for it, the Announce broadcasts that tell
///        it who is on the LAN, and nothing this relay says itself.

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "driver/uart.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "log/console_sink_posix.hpp"
#include "log/module.hpp"
#include "log/wire.hpp"
#include "log_modules.hpp"
#include "ota/updater.hpp"
#include "protocol/envelope.hpp"
#include "transport/node_id.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"
#include "transport/udp_link.hpp"

#include "firmware_store_esp32.hpp"

namespace mark4
{
    namespace
    {
        LogModule BOOT{LOG_MODULE_APP_BOOT, "app/boot"};
        LogModule WIFI{LOG_MODULE_APP_WIFI, "app/wifi"};
        LogModule CORE{LOG_MODULE_RELAY_CORE, "relay/core"};
        LogModule STATS{LOG_MODULE_RELAY_STATS, "relay/stats"};
        LogModule OTA{LOG_MODULE_RELAY_OTA, "relay/ota"};

        /// The console of the module, over its USB serial port: the same line
        /// format as any desktop node of the project, over the same stdio.
        ConsoleSinkPosix CONSOLE;

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

        /// Index of the LAN link, and the send mask naming it alone: what
        /// this node says itself (its log lines, its module table) is for the
        /// LAN and never for the board's line. Its beacon takes both links.
        constexpr std::size_t LAN_LINK = 1U;
        constexpr std::uint32_t LAN_ONLY = 1U << LAN_LINK;

        /// Poll cadence: one FreeRTOS tick, 1 ms with CONFIG_FREERTOS_HZ=1000.
        /// A 160-byte frame takes 1.7 ms on the line, so the added latency
        /// is under one frame time and every poll drains what arrived.
        constexpr TickType_t POLL_PERIOD_TICKS = 1U;

        /// Cadence of the statistics line [us].
        constexpr std::int64_t STATS_PERIOD_US = 5'000'000;

        /// Time left to the LAN link between a reboot request and the reset,
        /// so the log line announcing it leaves the radio.
        constexpr std::uint32_t REBOOT_GRACE_MS = 50U;

        /// Bytes of a MAC address.
        constexpr std::size_t MAC_SIZE = 6U;

        /// Bytes of the MAC the node name carries, its low half.
        constexpr std::size_t MAC_NAME_BYTES = 3U;

        /// @param tag body tag read off an encoded Envelope
        /// @return true when this node answers that body: the LogControl, the
        ///         Reboot and the updater messages a hub addresses to it.
        ///         Every other payload the delivery hands over is a broadcast
        ///         passing by (every broadcast of the LAN and of the board
        ///         crosses the delivery) and is dropped before any decoding.
        constexpr bool answeredHere(std::uint32_t tag)
        {
            switch (tag)
            {
                case mark4_Envelope_log_control_tag:
                case mark4_Envelope_reboot_tag:
                case mark4_Envelope_ota_status_request_tag:
                case mark4_Envelope_ota_begin_tag:
                case mark4_Envelope_ota_chunk_tag:
                case mark4_Envelope_ota_finish_tag:
                case mark4_Envelope_ota_revert_tag:
                case mark4_Envelope_ota_abort_tag:
                    return true;
                default:
                    return false;
            }
        }

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

        /// @brief Route of this node's own log lines and module table, defined
        ///        below the composition it reads.
        bool sendLogLine(void *context, const std::uint8_t *data, std::size_t size);

        /// The composition: services as members, declaration order is
        /// construction order, dependencies by reference.
        struct Relay
        {
            UartStream stream;                         ///< the UART driver rings
            UartLink uart{stream};                     ///< the board's link, serial framing
            UdpLink lan;                               ///< the WiFi LAN, discovery port 47820
            Transport transport;                       ///< the node, relaying between the two
            TransportSink logSink{&sendLogLine, this}; ///< its lines, onto the LAN
            FirmwareStoreEsp32 store;                  ///< the two OTA partitions
            OtaUpdater updater{store};                 ///< the update session over them
            bool storeReady = false;                   ///< the partition table is the two-slot one
            bool sessionWasOpen = false; ///< updater state at the last poll, for the log

            /// @param nodeId this relay's transport identity
            explicit Relay(std::uint32_t nodeId)
                : transport(nodeId)
            {
            }
        };

        bool sendLogLine(void *context, const std::uint8_t *data, std::size_t size)
        {
            return static_cast<Relay *>(context)->transport.send(
                BROADCAST_NODE, data, size, LAN_ONLY);
        }

        /// @brief Publishes this node's module table, as any node does after
        ///        its first beacon and on every level change.
        /// @param context the composition
        void publishModules(void *context)
        {
            static_cast<void>(logPublishModules(&sendLogLine, context));
        }

        /// @param context unused
        /// @return the instant the log records are stamped with [us]
        std::uint64_t logClock(void *context)
        {
            static_cast<void>(context);
            return static_cast<std::uint64_t>(esp_timer_get_time());
        }

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

        /// @brief Sends one Envelope to one node, encoded on the stack.
        /// @param relay the composition
        /// @param dst node to reach
        /// @param envelope message to send
        void sendTo(Relay &relay, std::uint32_t dst, const mark4_Envelope &envelope)
        {
            std::array<std::uint8_t, MAX_ENVELOPE_SIZE> bytes{};
            std::size_t size = 0U;
            if (encodeEnvelope(envelope, bytes.data(), bytes.size(), size))
            {
                static_cast<void>(relay.transport.send(dst, bytes.data(), size));
            }
        }

        /// @brief Offers a decoded message to the updater and answers it.
        /// @param relay the composition
        /// @param src node that sent it, where the reply goes
        /// @param envelope decoded message
        /// @return true when the updater claimed it, whatever it answered
        bool serveOta(Relay &relay, std::uint32_t src, const mark4_Envelope &envelope)
        {
            if (!relay.storeReady)
            {
                return false;
            }
            // A radio has nothing to arm and no battery floor to watch; the
            // one fact that matters is the clock the session timeout runs on.
            OtaUpdater::Inputs inputs;
            inputs.nowUs = static_cast<std::uint64_t>(esp_timer_get_time());
            mark4_Envelope reply;
            const bool consumed = relay.updater.handle(envelope, inputs, reply);
            if (reply.which_body != 0U)
            {
                sendTo(relay, src, reply);
            }
            if (reply.which_body == mark4_Envelope_ota_ack_tag &&
                reply.body.ota_ack.result != mark4_OtaResult_OTA_OK)
            {
                OTA.warn("op %d refused: result %d",
                         static_cast<int>(reply.body.ota_ack.op),
                         static_cast<int>(reply.body.ota_ack.result));
            }
            return consumed;
        }

        /// @brief Takes what the transport delivers to this node: the
        ///        LogControl a client sends it, the updater messages and the
        ///        Reboot of an update session. Every other payload is a
        ///        broadcast passing by and is dropped on its tag, before any
        ///        decoding.
        /// @param context the composition
        /// @param src node that sent it
        /// @param payload encoded Envelope
        /// @param size its size
        void onPayload(void *context,
                       std::uint32_t src,
                       const std::uint8_t *payload,
                       std::size_t size)
        {
            Relay &relay = *static_cast<Relay *>(context);
            if (!answeredHere(envelopeBodyTag(payload, size)))
            {
                return;
            }
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            if (!decodeEnvelope(payload, size, envelope))
            {
                return;
            }
            if (serveOta(relay, src, envelope))
            {
                return;
            }
            switch (envelope.which_body)
            {
                case mark4_Envelope_log_control_tag:
                    if (logHandleControl(envelope.body.log_control))
                    {
                        publishModules(context);
                    }
                    break;
                case mark4_Envelope_reboot_tag:
                    // The end of an update session: a staged image boots as
                    // the IDF bootloader's one-shot trial, anything else boots
                    // what runs today. The line goes out before the reset.
                    OTA.warn("reboot requested by %08" PRIx32, src);
                    vTaskDelay(pdMS_TO_TICKS(REBOOT_GRACE_MS));
                    esp_restart();
                    break;
                default:
                    break;
            }
        }

        /// @brief Logs a node the transport just heard for the first time.
        void onNodeUp(void *context, const Transport::Node &node)
        {
            static_cast<void>(context);
            if (node.link == UART_LINK)
            {
                CORE.info("node %08" PRIx32 " up on the uart", node.id);
                return;
            }
            CORE.info("node %08" PRIx32 " up on the lan at %u.%u.%u.%u:%u",
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
            CORE.info("node %08" PRIx32 " gone", node.id);
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

        /// @brief Registers this node's beacon: what it is, what it was built
        ///        from and the schema it speaks. It goes out on both links,
        ///        so the board learns this node like the LAN does.
        /// @param relay the composition
        /// @param mac the WiFi MAC, the node name's low half
        /// @return false when the announce does not fit a beacon
        bool setBeacon(Relay &relay, const std::array<std::uint8_t, MAC_SIZE> &mac)
        {
            mark4_Envelope announce = mark4_Envelope_init_zero;
            announce.which_body = mark4_Envelope_announce_tag;
            mark4_Announce &body = announce.body.announce;
            body.kind = mark4_NodeKind_RELAY;
            static_cast<void>(std::snprintf(body.name,
                                            sizeof(body.name),
                                            "relay-%02x%02x%02x",
                                            mac[MAC_SIZE - MAC_NAME_BYTES],
                                            mac[MAC_SIZE - 2U],
                                            mac[MAC_SIZE - 1U]));
            body.mcu = mark4_Mcu_ESP32C3;
            body.build_epoch = BRIDGE_BUILD_EPOCH;
            static_cast<void>(
                std::snprintf(body.git_hash, sizeof(body.git_hash), "%s", BRIDGE_GIT_HASH));
            body.wire_hash = WIRE_HASH;

            std::array<std::uint8_t, Transport::MAX_BEACON_SIZE> beacon{};
            std::size_t beaconSize = 0U;
            if (!encodeEnvelope(announce, beacon.data(), beacon.size(), beaconSize))
            {
                return false;
            }
            relay.transport.setBeacon(beacon.data(), beaconSize);
            return true;
        }
    } // namespace
} // namespace mark4

/// @brief Brings the logging up before anything has a line to say. Called
///        first thing by app_main(); the wire sink joins in relayRun(), once
///        the transport exists.
extern "C" void relayLogInit(void)
{
    using namespace mark4;
    logSetClock(&logClock, nullptr);
    static_cast<void>(logAddSink(CONSOLE));
}

/// @brief One line of the network bring-up (bridge_main.c, C), through the
///        app/wifi module.
extern "C" void bridgeLogInfo(const char *format, ...)
{
    using namespace mark4;
    va_list args;
    va_start(args, format);
    WIFI.vlog(LogLevel::INFO, format, args);
    va_end(args);
}

/// @brief Same, for what the bring-up did not get: a network that did not
///        answer, a station that dropped.
extern "C" void bridgeLogWarn(const char *format, ...)
{
    using namespace mark4;
    va_list args;
    va_start(args, format);
    WIFI.vlog(LogLevel::WARN, format, args);
    va_end(args);
}

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

    // A relay whose flash is not laid out for two slots still relays; it
    // only refuses to update itself, and says so once.
    relay.storeReady = relay.store.init();
    if (!relay.storeReady)
    {
        BOOT.error("no two-slot partition table: the relay cannot update over the air");
    }

    startUart();
    if (!relay.lan.init() || !relay.transport.addLink(relay.uart) ||
        !relay.transport.addLink(relay.lan) || !relay.transport.init())
    {
        BOOT.error("cannot start the transport");
        std::abort();
    }
    // lwIP has no getifaddrs(): the LAN link is told the one address a
    // broadcast of ours can come back from, so the echo is dropped instead
    // of counted as a duplicate of every frame relayed.
    relay.lan.addLocalHost(ownAddress());
    relay.transport.setRelay(true);
    relay.transport.setRelayFilter(&uartFilter, nullptr);
    relay.transport.setNodeCallbacks(&onNodeUp, &onNodeDown, nullptr);
    if (!setBeacon(relay, mac))
    {
        BOOT.error("the announce does not fit a beacon");
        std::abort();
    }
    static_cast<void>(logAddSink(relay.logSink));
    BOOT.info("boot: node %08" PRIx32 " relay build %lu %s wire %08lx",
              relay.transport.nodeId(),
              static_cast<unsigned long>(BRIDGE_BUILD_EPOCH),
              BRIDGE_GIT_HASH,
              static_cast<unsigned long>(WIRE_HASH));
    BOOT.info("uart %d baud, lan data port %u, discovery port %u",
              UART_BAUD,
              static_cast<unsigned>(relay.lan.dataPort()),
              static_cast<unsigned>(relay.lan.discoveryPort()));

    std::int64_t nextStatsUs = esp_timer_get_time() + STATS_PERIOD_US;
    bool modulesPublished = false;
    for (;;)
    {
        // The delivery takes what is addressed to this node (LogControl,
        // updater messages, Reboot); every other payload is relayed or
        // dropped by the transport.
        relay.transport.poll(static_cast<std::uint64_t>(esp_timer_get_time()), &onPayload, &relay);
        if (relay.storeReady)
        {
            relay.updater.tick(static_cast<std::uint64_t>(esp_timer_get_time()));
            if (relay.updater.sessionActive() != relay.sessionWasOpen)
            {
                relay.sessionWasOpen = relay.updater.sessionActive();
                OTA.info(relay.sessionWasOpen ? "update session open" : "update session closed");
            }
        }
        if (!modulesPublished)
        {
            // The first poll sent the first beacon: the table follows it.
            modulesPublished = true;
            publishModules(&relay);
        }
        const std::int64_t nowUs = esp_timer_get_time();
        if (nowUs >= nextStatsUs)
        {
            nextStatsUs = nowUs + STATS_PERIOD_US;
            STATS.debug("nodes %u, relayed %" PRIu32 ", filtered %" PRIu32 ", dropped %" PRIu32
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
