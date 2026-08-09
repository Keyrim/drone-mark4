/// @file
/// @brief Serial transport implementation. Raw termios, non-blocking reads,
///        drained from the single poll loop of the hub.

#include "hub/serial_transport.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace mark4
{
    namespace
    {
        /// One supported line speed and the termios constant naming it.
        struct BaudEntry
        {
            std::uint32_t baud; ///< speed in bauds
            speed_t constant;   ///< matching termios value
        };

        /// Speeds the bench actually uses. Anything else is refused rather
        /// than silently rounded to a speed the board does not speak.
        constexpr std::array<BaudEntry, 8U> BAUD_TABLE = {{{9600U, B9600},
                                                           {19200U, B19200},
                                                           {38400U, B38400},
                                                           {57600U, B57600},
                                                           {115200U, B115200},
                                                           {230400U, B230400},
                                                           {460800U, B460800},
                                                           {921600U, B921600}}};

        /// @brief Looks up the termios constant of a line speed.
        /// @param baud speed in bauds
        /// @param constantOut receives the constant
        /// @return true when the speed is supported
        bool baudConstant(std::uint32_t baud, speed_t &constantOut)
        {
            for (const BaudEntry &entry : BAUD_TABLE)
            {
                if (entry.baud == baud)
                {
                    constantOut = entry.constant;
                    return true;
                }
            }
            return false;
        }
    } // namespace

    SerialTransport::~SerialTransport()
    {
        close();
    }

    bool SerialTransport::open(const std::string &device, std::uint32_t baud)
    {
        close();
        m_device = device;
        m_baud = baud;
        return openPort(true);
    }

    bool SerialTransport::openPort(bool reportFailure)
    {
        close();

        speed_t speed = B0;
        if (!baudConstant(m_baud, speed))
        {
            static_cast<void>(std::fprintf(stderr, "hub: unsupported serial speed %u\n", m_baud));
            return false;
        }

        const int fd = ::open(m_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
        {
            if (reportFailure)
            {
                static_cast<void>(std::fprintf(
                    stderr, "hub: cannot open %s: %s\n", m_device.c_str(), std::strerror(errno)));
            }
            return false;
        }

        termios attributes{};
        if (::tcgetattr(fd, &attributes) < 0)
        {
            if (reportFailure)
            {
                static_cast<void>(std::fprintf(stderr,
                                               "hub: %s is not a serial port: %s\n",
                                               m_device.c_str(),
                                               std::strerror(errno)));
            }
            static_cast<void>(::close(fd));
            return false;
        }
        // Raw on both directions: the framing of protocol/ is the only
        // structure on this line, and a driver must not touch a single byte.
        attributes.c_iflag = 0;
        attributes.c_oflag = 0;
        attributes.c_lflag = 0;
        attributes.c_cflag = CREAD | CLOCAL | CS8;
        attributes.c_cc[VMIN] = 0;
        attributes.c_cc[VTIME] = 0;
        static_cast<void>(::cfsetispeed(&attributes, speed));
        static_cast<void>(::cfsetospeed(&attributes, speed));
        if (::tcsetattr(fd, TCSAFLUSH, &attributes) < 0)
        {
            if (reportFailure)
            {
                static_cast<void>(std::fprintf(stderr,
                                               "hub: cannot configure %s: %s\n",
                                               m_device.c_str(),
                                               std::strerror(errno)));
            }
            static_cast<void>(::close(fd));
            return false;
        }

        m_fd = fd;
        m_parser = SerialFrameParser{};
        return true;
    }

    void SerialTransport::close()
    {
        if (m_fd >= 0)
        {
            static_cast<void>(::close(m_fd));
            m_fd = -1;
        }
    }

    void SerialTransport::drain(const PayloadHandler &onPayload)
    {
        if (m_fd < 0)
        {
            return;
        }
        std::array<std::uint8_t, READ_CHUNK> chunk{};
        while (true)
        {
            const ssize_t read = ::read(m_fd, chunk.data(), chunk.size());
            if (read < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                {
                    return;
                }
                static_cast<void>(std::fprintf(stderr,
                                               "hub: %s went away (%s), will reopen\n",
                                               m_device.c_str(),
                                               std::strerror(errno)));
                close();
                return;
            }
            if (read == 0)
            {
                return;
            }
            for (ssize_t index = 0; index < read; ++index)
            {
                const std::size_t size = m_parser.feed(chunk[static_cast<std::size_t>(index)]);
                if (size > 0U)
                {
                    onPayload(m_parser.payload(), size);
                }
            }
            if (static_cast<std::size_t>(read) < chunk.size())
            {
                return;
            }
        }
    }

    void SerialTransport::maintain(std::uint64_t nowMs)
    {
        if (m_fd >= 0 || m_device.empty())
        {
            return;
        }
        if (nowMs < m_reopenAtMs)
        {
            return;
        }
        m_reopenAtMs = nowMs + REOPEN_PERIOD_MS;
        if (openPort(false))
        {
            static_cast<void>(std::printf("hub: %s reopened\n", m_device.c_str()));
            static_cast<void>(std::fflush(stdout));
        }
    }

    bool SerialTransport::sendPacket(const std::uint8_t *payload, std::size_t size) const
    {
        if (m_fd < 0)
        {
            return false;
        }
        std::array<std::uint8_t, SERIAL_MAX_PAYLOAD + SERIAL_FRAME_OVERHEAD> frame{};
        const std::size_t framed = encodeSerialFrame(payload, size, frame.data());
        if (framed == 0U)
        {
            return false;
        }
        std::size_t written = 0U;
        while (written < framed)
        {
            const ssize_t chunk = ::write(m_fd, &frame[written], framed - written);
            if (chunk <= 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            written += static_cast<std::size_t>(chunk);
        }
        return true;
    }
} // namespace mark4
