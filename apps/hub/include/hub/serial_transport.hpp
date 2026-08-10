#pragma once

/// @file
/// @brief The serial side of the hub: the single owner of the UART the real
///        board is wired to. Everything else in the system stays on the UDP
///        boundary of protocol/, exactly as it will when an ESP32 bridge
///        replaces this cable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "protocol/serial_framing.hpp"

namespace mark4
{
    /// One raw serial link, framed with the stream framing of protocol/.
    /// A board that reboots, or a cable pulled out, closes the link; the hub
    /// keeps running and reopens it on its own.
    class SerialTransport
    {
      public:
        /// Delay between two attempts to reopen a link that went away [ms].
        static constexpr std::uint64_t REOPEN_PERIOD_MS = 2000U;

        /// Bytes read from the port per drain().
        static constexpr std::size_t READ_CHUNK = 4096U;

        /// Called once per frame whose CRC checked out.
        using PayloadHandler = std::function<void(const std::uint8_t *, std::size_t)>;

        SerialTransport() = default;
        SerialTransport(const SerialTransport &) = delete;
        SerialTransport &operator=(const SerialTransport &) = delete;
        SerialTransport(SerialTransport &&) = delete;
        SerialTransport &operator=(SerialTransport &&) = delete;
        ~SerialTransport();

        /// @brief Opens a serial port in raw non-blocking mode. The device is
        ///        remembered, so a later maintain() can reopen it alone.
        /// @param device device path, for instance /dev/ttyUSB0
        /// @param baud line speed in bauds
        /// @return true when the port is open and configured
        bool open(const std::string &device, std::uint32_t baud);

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
        /// @brief Opens the configured device.
        /// @param reportFailure true to log why the open failed; the periodic
        ///        reopen attempts stay silent, a missing board must not fill
        ///        the console with the same line every two seconds
        /// @return true when the port is open and configured
        bool openPort(bool reportFailure);

        std::string m_device;            ///< device path, empty when unconfigured
        std::uint32_t m_baud = 0U;       ///< configured line speed [baud]
        int m_fd = -1;                   ///< open descriptor, -1 when closed
        std::uint64_t m_reopenAtMs = 0U; ///< earliest next reopen attempt [ms]
        SerialFrameParser m_parser;      ///< incremental decoder of the downlink
    };
} // namespace mark4
