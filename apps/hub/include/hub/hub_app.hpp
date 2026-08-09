#pragma once

/// @file
/// @brief hub composition root.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hub/discovery.hpp"
#include "hub/json_codec.hpp"
#include "hub/serial_transport.hpp"
#include "hub/stream_recorder.hpp"
#include "hub/udp_transport.hpp"
#include "hub/ws_bridge.hpp"
#include "protocol/ports.hpp"

namespace mark4
{
    /// Composition root: owns every service as a value member. Member
    /// declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no
    /// manual teardown. Built by main(), passed by reference: no singleton.
    class HubApp
    {
      public:
        /// TCP port the websocket endpoint listens on. Deliberately not in
        /// protocol/ports.hpp: that file describes the UDP boundaries between
        /// processes speaking the binary wire, and this is neither.
        static constexpr std::uint16_t WS_PORT = 47810U;

        /// How long the poll loop sleeps when nothing is happening [ms].
        static constexpr int POLL_TIMEOUT_MS = 20;

        /// Silence after which an announced process is declared gone [us].
        /// The announce cadence contract is one per second, first one
        /// immediate, so this is three missed announces.
        static constexpr std::uint64_t DISCOVERY_EXPIRY_US = 3'000'000U;

        /// Period of the status message [ms].
        static constexpr std::uint64_t STATUS_PERIOD_MS = 1000U;

        /// Default line speed of the board UART [baud].
        static constexpr std::uint32_t DEFAULT_SERIAL_BAUD = 921600U;

        /// Everything main() decides before the hub starts.
        struct Config
        {
            std::uint16_t wsPort = WS_PORT;                  ///< websocket endpoint port
            std::uint16_t announcePort = ANNOUNCE_PORT;      ///< announce listen port
            std::uint16_t telemetryPort = TELEMETRY_PORT;    ///< telemetry port watched by default
            std::uint16_t simRawPort = SIM_RAW_PORT;         ///< sim raw port watched by default
            std::uint16_t simCommandPort = SIM_COMMAND_PORT; ///< scenario commands go there
            std::string serialDevice;                        ///< board UART, empty = none
            std::uint32_t serialBaud = DEFAULT_SERIAL_BAUD;  ///< board UART speed [baud]
            std::string logDirectory = "logs";               ///< where recordings are written
            bool recordOnStart = false;                      ///< open a CSV session at startup
            bool udpRebroadcast = true;                      ///< re-emit serial telemetry on UDP
        };

        /// @param config settings of this run
        explicit HubApp(Config config);

        /// @brief Initializes services in declaration order: opens the UDP
        ///        sockets, the serial port and the recording. The first
        ///        failure is logged and returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the poll loop until a stop is requested or keepRunning
        ///        returns false.
        /// @param keepRunning polled once per loop; returning false ends the run
        /// @return process exit code
        int run(const std::function<bool()> &keepRunning);

        /// @brief Asks the loop to stop. Signal-safe: sets one atomic flag.
        void requestStop()
        {
            m_stopRequested.store(true);
        }

        /// @return serial link, so a caller that requires the board can check it
        [[nodiscard]] const SerialTransport &accessSerial() const
        {
            return m_serial;
        }

        /// @return recorder, for post-run reporting
        [[nodiscard]] const StreamRecorder &accessRecorder() const
        {
            return m_recorder;
        }

      private:
        /// @brief Handles one datagram read from a listening socket.
        /// @param localPort port the datagram landed on
        /// @param data datagram bytes
        /// @param size datagram size
        void onDatagram(std::uint16_t localPort, const std::uint8_t *data, std::size_t size);

        /// @brief Handles one CRC-valid frame read from the serial link.
        /// @param payload frame payload
        /// @param size payload size
        void onSerialPayload(const std::uint8_t *payload, std::size_t size);

        /// @brief Routes one telemetry packet to the recorder and the clients.
        /// @param data packet bytes
        void onTelemetryPacket(const std::uint8_t *data);

        /// @brief Routes one raw simulator packet to the recorder and clients.
        /// @param data packet bytes
        void onSimRawPacket(const std::uint8_t *data);

        /// @brief Renders one tuning answer to the clients.
        /// @param data packet bytes
        /// @param size packet size in bytes
        /// @param source process the answer came from, inferred from the path
        ///        it arrived by
        /// @return true when the packet was a tuning answer
        bool onTuningAnswer(const std::uint8_t *data, std::size_t size, StreamSource source);

        /// @brief Sends one command packet to a process, by the route that
        ///        kind of process is reachable on: the board is at the end of
        ///        the serial cable, everything else at the command port it
        ///        announced.
        /// @param target kind of process to reach
        /// @param data packet bytes
        /// @param size packet size in bytes
        /// @param errorOut receives the reason when the target is unreachable
        /// @return true when the bytes went out
        bool sendToTarget(StreamSource target,
                          const std::uint8_t *data,
                          std::size_t size,
                          std::string &errorOut);

        /// @brief Applies one discovery event: follows the telemetry port the
        ///        process serves, and tells the clients what changed.
        /// @param change event to apply
        void onDiscoveryChange(const DiscoveryChange &change);

        /// @brief Starts listening on a telemetry port one more process needs.
        /// @param port port to follow, 0 to do nothing
        void retainPort(std::uint16_t port);

        /// @brief Stops listening on a telemetry port nobody needs any more.
        /// @param port port to release, 0 to do nothing
        void releasePort(std::uint16_t port);

        /// @brief Decodes and routes everything the clients have sent.
        void handleClientMessages();

        /// @brief Carries out one decoded client request.
        /// @param message request to carry out
        /// @param errorOut receives the refusal reason when it cannot be
        /// @return true when the request was carried out
        bool applyClientMessage(const ClientMessage &message, std::string &errorOut);

        /// @brief Answers one request, when the client asked to be answered.
        ///        The answer is broadcast: it carries the correlation id the
        ///        client sent, and a client ignores what is not its own.
        /// @param id correlation id, -1 for a request that wants no answer
        /// @param ok true when the request was carried out
        /// @param error refusal reason, empty when ok
        void answer(int id, bool ok, const std::string &error);

        /// @brief Sends the current discovery table to the clients.
        void broadcastDiscovery();

        /// @brief Sends the current counters to the clients.
        void broadcastStatus();

        /// @brief Expiry, status message and serial reopen.
        /// @param nowUs current time [us]
        void housekeeping(std::uint64_t nowUs);

        /// @return the counters and flags the status message carries
        [[nodiscard]] HubStatus status() const;

        /// One telemetry port and the number of live processes serving it.
        struct PortUse
        {
            std::uint16_t port = 0U; ///< followed port
            unsigned users = 0U;     ///< live processes needing it, 0 = pinned by the config
        };

        Config m_config;                         ///< settings of this run
        StreamRecorder m_recorder;               ///< CSV pair and blackbox file
        DiscoveryRegistry m_registry;            ///< live processes
        UdpTransport m_udp;                      ///< every UDP socket
        SerialTransport m_serial;                ///< board UART, when one is configured
        WsBridge m_ws;                           ///< websocket endpoint
        std::vector<PortUse> m_followedPorts;    ///< refcount behind every subscription
        std::atomic_bool m_stopRequested{false}; ///< set by a signal handler
        std::uint64_t m_nextStatusUs = 0U;       ///< next status message instant [us]
    };
} // namespace mark4
