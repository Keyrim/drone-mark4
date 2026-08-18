/// @file
/// @brief Serial transport implementation. Raw termios, non-blocking reads,
///        drained from the single poll loop of the hub.

#include "hub/serial_transport.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

        m_isDatagram = m_device.rfind(UDP_PREFIX, 0U) == 0U;
        if (m_isDatagram)
        {
            return openDatagram(reportFailure);
        }

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

    bool SerialTransport::openDatagram(bool reportFailure)
    {
        const std::string target = m_device.substr(std::strlen(UDP_PREFIX));
        const std::size_t colon = target.rfind(':');
        sockaddr_in address{};
        address.sin_family = AF_INET;
        const unsigned long port = colon == std::string::npos
                                       ? 0UL
                                       : std::strtoul(target.c_str() + colon + 1U, nullptr, 10);
        if (colon == std::string::npos || port == 0UL || port > UINT16_MAX ||
            ::inet_pton(AF_INET, target.substr(0U, colon).c_str(), &address.sin_addr) != 1)
        {
            static_cast<void>(
                std::fprintf(stderr, "hub: %s is not udp:address:port\n", m_device.c_str()));
            return false;
        }
        address.sin_port = htons(static_cast<std::uint16_t>(port));

        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (fd < 0)
        {
            if (reportFailure)
            {
                static_cast<void>(
                    std::fprintf(stderr, "hub: cannot open a socket: %s\n", std::strerror(errno)));
            }
            return false;
        }
        // Connecting a datagram socket picks the route, fixes the source
        // address and filters what comes back to that one peer. It also makes
        // read() and write() work, so drain() and sendPacket() do not care
        // which kind of link they are on.
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        {
            if (reportFailure)
            {
                static_cast<void>(std::fprintf(
                    stderr, "hub: cannot reach %s: %s\n", m_device.c_str(), std::strerror(errno)));
            }
            static_cast<void>(::close(fd));
            return false;
        }

        m_fd = fd;
        m_helloAtMs = 0U;
        m_parser = SerialFrameParser{};
        return true;
    }

    bool SerialTransport::sendHello() const
    {
        // One byte the framing can only skip: the bridge reads the address it
        // came from and drops it, and a board that ever saw it would discard
        // it while hunting for the sync pair.
        const std::uint8_t hello = 0U;
        return ::write(m_fd, &hello, sizeof(hello)) == static_cast<ssize_t>(sizeof(hello));
    }

    void SerialTransport::close()
    {
        if (m_fd >= 0)
        {
            static_cast<void>(::close(m_fd));
            m_fd = -1;
        }
    }

    void SerialTransport::release()
    {
        close();
        m_device.clear();
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
                // ECONNREFUSED is the bridge answering an ICMP error for a
                // datagram we sent while it was down: the socket stays usable.
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                    errno == ECONNREFUSED)
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
        if (m_fd >= 0)
        {
            // Nothing keeps a datagram link alive but the hello: the bridge
            // forgets where to send when we stop, and a hello that cannot go
            // out means the network went away, which reopening will heal.
            if (m_isDatagram && nowMs >= m_helloAtMs)
            {
                m_helloAtMs = nowMs + HELLO_PERIOD_MS;
                if (!sendHello())
                {
                    static_cast<void>(std::fprintf(stderr,
                                                   "hub: %s went away (%s), will reopen\n",
                                                   m_device.c_str(),
                                                   std::strerror(errno)));
                    close();
                }
            }
            return;
        }
        if (m_device.empty())
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
