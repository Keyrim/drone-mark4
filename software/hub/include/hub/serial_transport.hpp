#pragma once

/// @file
/// @brief The serial side of the hub: the single owner of the link to the
///        real board. The bytes reach this machine as datagrams from the
///        ESP32 bridge, and carry the stream framing of protocol/ from end
///        to end.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "transport/serial_framing.hpp"

namespace mark4
{
    /// One raw serial link, framed with the stream framing of protocol/.
    /// A board that reboots, or a bridge that goes away, closes the link;
    /// the hub keeps running and reopens it on its own.
    class SerialTransport
    {
      public:
        /// Delay between two attempts to reopen a link that went away [ms].
        static constexpr std::uint64_t REOPEN_PERIOD_MS = 2000U;

        /// Bytes read from the port per drain().
        static constexpr std::size_t READ_CHUNK = 4096U;

        /// Device prefix naming the WiFi bridge, as in
        /// udp:192.168.4.1:47830. Nothing else is a valid device.
        static constexpr const char *UDP_PREFIX = "udp:";

        /// Delay between two hello datagrams [ms]. The bridge sends its
        /// downlink to whoever last said hello, so this is what a bridge that
        /// rebooted waits before speaking again.
        static constexpr std::uint64_t HELLO_PERIOD_MS = 1000U;

        /// Called once per frame whose CRC checked out.
        using PayloadHandler = std::function<void(const std::uint8_t *, std::size_t)>;

        SerialTransport() = default;
        SerialTransport(const SerialTransport &) = delete;
        SerialTransport &operator=(const SerialTransport &) = delete;
        SerialTransport(SerialTransport &&) = delete;
        SerialTransport &operator=(SerialTransport &&) = delete;
        ~SerialTransport();

        /// @brief Opens the link in non-blocking mode. The device is
        ///        remembered, so a later maintain() can reopen it alone.
        /// @param device bridge device, as in udp:192.168.4.1:47830
        /// @return true when the link is open and configured
        bool open(const std::string &device);

        /// @brief Closes the port, if it is open.
        void close();

        /// @brief Closes the port and forgets the device: maintain() stops
        ///        reopening it. This is the deliberate operator close, where
        ///        close() is the transient one a read error takes.
        void release();

        /// @return true when the port is usable
        [[nodiscard]] bool isOpen() const
        {
            return m_fd >= 0;
        }

        /// @return descriptor to poll, -1 when the port is closed
        [[nodiscard]] int fd() const
        {
            return m_fd;
        }

        /// @return device path the transport was configured with
        [[nodiscard]] const std::string &device() const
        {
            return m_device;
        }

        /// @brief Reads what the port has and reports every complete frame.
        ///        A read error closes the port and schedules a reopen.
        /// @param onPayload called once per CRC-valid frame
        void drain(const PayloadHandler &onPayload);

        /// @brief Reopens a closed port when the retry delay has elapsed.
        /// @param nowMs current time [ms]
        void maintain(std::uint64_t nowMs);

        /// @brief Frames one packet and writes it to the port. Const because
        ///        what changes is the state of the port in the kernel, not
        ///        this object.
        /// @param payload packet bytes
        /// @param size packet size
        /// @return true when the whole frame was written
        bool sendPacket(const std::uint8_t *payload, std::size_t size) const;

      private:
        /// @brief Opens the UDP link the configured device names. The
        ///        socket is connected, so the rest of the class treats the
        ///        link as one descriptor to read from and write to.
        /// @param reportFailure true to log why the open failed; the periodic
        ///        reopen attempts stay silent, a missing bridge must not fill
        ///        the console with the same line every two seconds
        /// @return true when the link is usable
        bool openDatagram(bool reportFailure);

        /// @brief Sends the datagram the bridge learns our address from.
        /// @return true when it went out
        [[nodiscard]] bool sendHello() const;

        std::string m_device;            ///< bridge device, empty when unconfigured
        int m_fd = -1;                   ///< open descriptor, -1 when closed
        std::uint64_t m_helloAtMs = 0U;  ///< earliest next hello datagram [ms]
        std::uint64_t m_reopenAtMs = 0U; ///< earliest next reopen attempt [ms]
        SerialFrameParser m_parser;      ///< incremental decoder of the downlink
    };
} // namespace mark4
