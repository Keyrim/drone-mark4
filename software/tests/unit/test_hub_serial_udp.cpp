/// @file
/// @brief The serial transport over the WiFi bridge: a udp: device carries
///        the same framed stream a cable does. A socket standing in for the
///        bridge answers the hello and replies with one framed packet.

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "hub/serial_transport.hpp"
#include "protocol/serial_framing.hpp"

namespace
{
    /// Port the fake bridge binds on the loopback. Not the port of a real
    /// bridge, and not the announce port either: a hub running next to the
    /// test listens on that one, and the test would fail against it.
    constexpr std::uint16_t BRIDGE_PORT = 47839U;

    /// A socket bound to BRIDGE_PORT, standing in for the ESP32 bridge.
    class FakeBridge
    {
      public:
        FakeBridge()
        {
            m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            local.sin_port = htons(BRIDGE_PORT);
            m_bound = m_fd >= 0 &&
                      ::bind(m_fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) == 0;
        }
        FakeBridge(const FakeBridge &) = delete;
        FakeBridge &operator=(const FakeBridge &) = delete;
        FakeBridge(FakeBridge &&) = delete;
        FakeBridge &operator=(FakeBridge &&) = delete;
        ~FakeBridge()
        {
            if (m_fd >= 0)
            {
                static_cast<void>(::close(m_fd));
            }
        }

        [[nodiscard]] bool isBound() const
        {
            return m_bound;
        }

        /// @brief Waits for one datagram and remembers who sent it.
        /// @return true when one arrived
        bool learnPeer()
        {
            std::array<std::uint8_t, 64U> scratch{};
            socklen_t length = sizeof(m_peer);
            return ::recvfrom(m_fd,
                              scratch.data(),
                              scratch.size(),
                              0,
                              reinterpret_cast<sockaddr *>(&m_peer),
                              &length) > 0;
        }

        /// @brief Reads one datagram, the way the bridge reads what it has
        ///        to write on the UART of the board.
        /// @return the bytes, empty when nothing came within the timeout
        std::vector<std::uint8_t> receive()
        {
            pollfd entry{m_fd, POLLIN, 0};
            if (::poll(&entry, 1U, 500) != 1)
            {
                return {};
            }
            std::array<std::uint8_t, 512U> scratch{};
            const ssize_t size = ::recv(m_fd, scratch.data(), scratch.size(), 0);
            return size > 0 ? std::vector<std::uint8_t>(scratch.begin(), scratch.begin() + size)
                            : std::vector<std::uint8_t>{};
        }

        /// @brief Sends bytes to the learned peer, as the bridge does with
        ///        whatever the board wrote on the UART.
        void send(const std::vector<std::uint8_t> &bytes)
        {
            static_cast<void>(::sendto(m_fd,
                                       bytes.data(),
                                       bytes.size(),
                                       0,
                                       reinterpret_cast<const sockaddr *>(&m_peer),
                                       sizeof(m_peer)));
        }

      private:
        int m_fd = -1;        ///< the socket
        bool m_bound = false; ///< true when the port was free
        sockaddr_in m_peer{}; ///< address the hello came from
    };
} // namespace

TEST_CASE("a udp device carries the framed stream of the bridge")
{
    FakeBridge bridge;
    REQUIRE(bridge.isBound());

    mark4::SerialTransport transport;
    const std::string device = "udp:127.0.0.1:" + std::to_string(BRIDGE_PORT);
    REQUIRE(transport.open(device));
    REQUIRE(transport.isOpen());

    // The hub speaks first: the bridge only knows where to send once it has
    // heard from the ground.
    transport.maintain(0U);
    REQUIRE(bridge.learnPeer());

    // One packet, framed exactly as it would be on the cable, split across
    // two datagrams to prove the parser does not care about boundaries.
    const std::array<std::uint8_t, 4U> payload = {0x01U, 0x02U, 0x03U, 0x04U};
    std::array<std::uint8_t, 32U> frame{};
    const std::size_t framed =
        mark4::encodeSerialFrame(payload.data(), payload.size(), frame.data());
    REQUIRE(framed == payload.size() + mark4::SERIAL_FRAME_OVERHEAD);
    bridge.send(std::vector<std::uint8_t>(frame.begin(), frame.begin() + 3));
    bridge.send(std::vector<std::uint8_t>(frame.begin() + 3, frame.begin() + framed));

    // Loopback delivery is not instant: the hub waits on the descriptor the
    // same way its poll loop does.
    std::vector<std::uint8_t> received;
    for (int attempt = 0; attempt < 20 && received.empty(); ++attempt)
    {
        pollfd entry{transport.fd(), POLLIN, 0};
        static_cast<void>(::poll(&entry, 1U, 100));
        transport.drain([&received](const std::uint8_t *data, std::size_t size) {
            received.assign(data, data + size);
        });
    }
    REQUIRE(received == std::vector<std::uint8_t>(payload.begin(), payload.end()));

    // The other direction: what the hub sends reaches the bridge framed, as
    // one datagram, ready to be written on the UART byte for byte.
    REQUIRE(transport.sendPacket(payload.data(), payload.size()));
    const std::vector<std::uint8_t> sent = bridge.receive();
    REQUIRE(sent == std::vector<std::uint8_t>(frame.begin(), frame.begin() + framed));
}

TEST_CASE("a malformed udp device is refused")
{
    mark4::SerialTransport transport;
    REQUIRE(!transport.open("udp:127.0.0.1"));
    REQUIRE(!transport.open("udp:not-an-address:47839"));
    REQUIRE(!transport.open("udp:127.0.0.1:0"));
    REQUIRE(!transport.isOpen());
}
