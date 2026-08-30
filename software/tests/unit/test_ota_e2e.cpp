/// @file
/// @brief The desktop end-to-end test of the firmware update system, the one
///        docs/ota-design.md section 6 asks for: the hub's real OtaClient
///        against a real drone_sim process, over real UDP, with a real .ota
///        bundle on disk. Nothing is faked between the two ends - the sim
///        runs the same OtaUpdater the board runs, over the file-backed store,
///        and its fake bootloader runs the same slot decision drone_boot does.
///
///        Two flows, in one sequence because the second one starts from the
///        state the first one left behind (that is what an emulated flash is
///        for): a full update that ends confirmed, then an update left
///        unconfirmed and rebooted, which must roll back to the image the
///        first one installed and leave the trial slot BAD.
///
///        Everything the test touches is a public surface: the OtaClient API,
///        drone_sim's command line, the wire, and the bundle file format.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "byte_pipe.hpp"
#include "hub/gateway_codec.hpp"
#include "hub/ota_bundle.hpp"
#include "hub/ota_client.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"
#include "transport/udp_link.hpp"

namespace
{
    /// Bytes of payload behind the header of every test image. A multiple of
    /// four, so the CRC convention's 0xFF tail padding never enters the
    /// picture, and small enough that the whole transfer is a handful of
    /// chunks.
    constexpr std::uint32_t IMAGE_PAYLOAD_SIZE = 1024U;

    /// Total bytes of one test image.
    constexpr std::uint32_t IMAGE_SIZE = mark4::OTA_IMAGE_HEADER_SIZE + IMAGE_PAYLOAD_SIZE;

    /// Longest the test waits for one phase transition [ms]. Generous: the
    /// sim only looks at its command uplink every IDLE_TIMEOUT_MS while no
    /// session is open, and under ASan everything is slower.
    constexpr std::uint64_t STEP_BUDGET_MS = 40000U;

    /// Poll period of the drive loop [us].
    constexpr std::uint64_t DRIVE_STEP_US = 500U;

    constexpr std::uint64_t US_PER_MS = 1000U;
    constexpr std::uint64_t NS_PER_US = 1000U;

    /// @return a monotonic microsecond clock, the one the client is fed
    std::uint64_t nowUs()
    {
        timespec now{};
        static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &now));
        return (static_cast<std::uint64_t>(now.tv_sec) * 1000000U) +
               (static_cast<std::uint64_t>(now.tv_nsec) / NS_PER_US);
    }

    /// @brief Sleeps a short while so the drive loop does not spin flat out.
    void sleepStep()
    {
        const timespec request = {0, static_cast<long>(DRIVE_STEP_US * NS_PER_US)};
        static_cast<void>(::nanosleep(&request, nullptr));
    }

    /// @brief Appends a little-endian 32-bit value.
    /// @param[out] bytesInOut buffer to append to
    /// @param value value to append
    void appendU32(std::vector<std::uint8_t> &bytesInOut, std::uint32_t value)
    {
        static constexpr std::uint32_t BYTE_MASK = 0xFFU;
        static constexpr std::uint32_t BITS_PER_BYTE = 8U;
        for (std::uint32_t shift = 0U; shift < 4U; ++shift)
        {
            bytesInOut.push_back(
                static_cast<std::uint8_t>((value >> (shift * BITS_PER_BYTE)) & BYTE_MASK));
        }
    }

    /// @brief Builds one complete image for a slot: a fully stamped
    ///        OtaImageHeader followed by a payload whose bytes depend on the
    ///        slot and the build epoch, so no two images of this test are alike.
    /// @param slot slot the image is linked for
    /// @param buildEpoch build identity the image announces
    /// @param gitHash eight-character build hash
    /// @param broken true stamps the payload with drone_sim's "notalive"
    ///        marker: the fake image boots but never reaches the checkpoint
    ///        where a trial confirms itself
    /// @return the image bytes
    std::vector<std::uint8_t> makeImage(std::uint8_t slot,
                                        std::uint32_t buildEpoch,
                                        const std::string &gitHash,
                                        bool broken = false)
    {
        std::vector<std::uint8_t> image(IMAGE_SIZE, 0xFFU);
        for (std::uint32_t i = 0U; i < IMAGE_PAYLOAD_SIZE; ++i)
        {
            image[mark4::OTA_IMAGE_HEADER_SIZE + i] =
                static_cast<std::uint8_t>(i + slot + buildEpoch);
        }
        if (broken)
        {
            static constexpr char MARKER[] = "notalive";
            std::memcpy(image.data() + mark4::OTA_IMAGE_HEADER_SIZE, MARKER, sizeof(MARKER) - 1U);
        }

        mark4::OtaImageHeader header{};
        header.magic = mark4::OTA_IMAGE_MAGIC;
        header.headerVersion = mark4::OTA_IMAGE_HEADER_VERSION;
        header.mcuId = mark4::OTA_MCU_SIM;
        header.slotId = slot;
        header.imageSize = IMAGE_SIZE;
        header.imageCrc =
            mark4::otaImageCrc32(image.data() + mark4::OTA_IMAGE_HEADER_SIZE, IMAGE_PAYLOAD_SIZE);
        header.buildEpoch = buildEpoch;
        header.gitHash.fill('\0');
        std::memcpy(header.gitHash.data(),
                    gitHash.data(),
                    std::min(gitHash.size(), mark4::OTA_GIT_HASH_SIZE));
        header.reserved.fill(0xFFU);
        header.headerCrc = 0U;
        std::memcpy(image.data(), &header, sizeof(header));

        // The header CRC covers the 508 bytes before itself and must be
        // stamped last, exactly like scripts/make_ota.py does it.
        const std::uint32_t headerCrc =
            mark4::otaImageCrc32(image.data(), offsetof(mark4::OtaImageHeader, headerCrc));
        std::memcpy(image.data() + offsetof(mark4::OtaImageHeader, headerCrc),
                    &headerCrc,
                    sizeof(headerCrc));
        return image;
    }

    /// @brief Writes a complete two-image .ota bundle for OTA_MCU_SIM. The
    ///        layout is the one hub/ota_bundle.hpp documents; the manifest is
    ///        the one scripts/make_ota.py writes, minus the F405 assumptions
    ///        that script is built on.
    /// @param path file to write
    /// @param buildEpoch build identity both images announce
    /// @param gitHash eight-character build hash
    /// @param broken true builds images that never confirm their own trial
    /// @return the bundle path, for chaining
    std::string writeBundle(const std::filesystem::path &path,
                            std::uint32_t buildEpoch,
                            const std::string &gitHash,
                            bool broken = false)
    {
        const std::vector<std::uint8_t> slotA =
            makeImage(mark4::OTA_SLOT_A, buildEpoch, gitHash, broken);
        const std::vector<std::uint8_t> slotB =
            makeImage(mark4::OTA_SLOT_B, buildEpoch, gitHash, broken);

        // The manifest reads exactly like the one scripts/make_ota.py writes:
        // compact, keys sorted, every number decimal.
        std::string manifest = R"({"buildEpoch":)" + std::to_string(buildEpoch) +
                               R"(,"gitHash":")" + gitHash + R"(","images":[)";
        manifest += R"({"crc32":)" +
                    std::to_string(mark4::otaImageCrc32(slotA.data(), slotA.size())) +
                    R"(,"size":)" + std::to_string(slotA.size()) + R"(,"slot":0},)";
        manifest += R"({"crc32":)" +
                    std::to_string(mark4::otaImageCrc32(slotB.data(), slotB.size())) +
                    R"(,"size":)" + std::to_string(slotB.size()) + R"(,"slot":1}],)";
        manifest += R"("mcuId":)" + std::to_string(mark4::OTA_MCU_SIM);
        std::array<char, 9U> wireHash{};
        static_cast<void>(
            std::snprintf(wireHash.data(), wireHash.size(), "%08x", mark4::WIRE_HASH));
        manifest += R"(,"name":"drone_sim","wireHash":")" + std::string(wireHash.data()) + R"("})";

        std::vector<std::uint8_t> bytes;
        const char *magic = mark4::otaBundleMagic();
        bytes.insert(bytes.end(), magic, magic + mark4::OTA_BUNDLE_MAGIC_SIZE);
        appendU32(bytes, static_cast<std::uint32_t>(manifest.size()));
        bytes.insert(bytes.end(), manifest.begin(), manifest.end());
        for (const std::vector<std::uint8_t> &image : {slotA, slotB})
        {
            appendU32(bytes, static_cast<std::uint32_t>(image.size()));
            bytes.insert(bytes.end(), image.begin(), image.end());
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        return path.string();
    }

    /// Transport identity of the ground side in this test.
    constexpr std::uint32_t GROUND_NODE = 0x6E0D0001U;

    /// Transport identity the sim is started with, so the ground side can
    /// address it before it ever announced anything but its beacon.
    constexpr std::uint32_t SIM_NODE = 0x51300001U;

    /// The ESP32's outbound rule on its UART link (index 0 of its
    /// transport): a broadcast only goes down the wire when it is an
    /// Announce; a unicast routed there is for the board by construction.
    bool uartFilter(void *context,
                    std::size_t linkIndex,
                    const mark4::FrameHeader &header,
                    const std::uint8_t *payload,
                    std::size_t size)
    {
        static_cast<void>(context);
        return linkIndex != 0U || header.dst != mark4::BROADCAST_NODE ||
               mark4::envelopeIsAnnounce(payload, size);
    }

    /// The ground side of the link, exactly what the hub is: one transport
    /// node addressing the sim by the node id it was started with. Two
    /// shapes: straight on the LAN (one UDP link on a private discovery
    /// port), or through a relay: the ground node stays on its LAN, an
    /// ESP32-like relay (UDP link on that LAN, UartLink on an in-memory
    /// wire, relay on, no beacon, the UART filter) stands where the ESP32
    /// stands, and a second relay on the far end of the wire (UartLink, UDP
    /// link on a second private LAN, no filter) stands where the board's
    /// transport stands with the sim behind it, so every updater message
    /// crosses the serial framing, the filter and two relays both ways.
    class GroundLink
    {
      public:
        /// Node the ESP32-like relay takes.
        static constexpr std::uint32_t RELAY_NODE = 0x4E1A4000U;

        /// Node the far relay (the board's side of the wire) takes.
        static constexpr std::uint32_t FAR_RELAY_NODE = 0x4E1A4001U;

        /// @param discoveryPort shared port of the ground LAN
        /// @param farPort shared port of the far LAN, 0 to go straight
        explicit GroundLink(std::uint16_t discoveryPort, std::uint16_t farPort = 0U)
            : m_viaRelay(farPort != 0U),
              m_udp(discoveryPort),
              m_relayUdp(discoveryPort),
              m_farUdp(farPort)
        {
        }

        /// @brief Opens the sockets and composes the transport(s).
        /// @return true when frames can flow
        bool open()
        {
            if (!m_udp.init() || !m_transport.addLink(m_udp) || !m_transport.init())
            {
                return false;
            }
            if (!m_viaRelay)
            {
                return true;
            }
            // The ground beacons like the hub does, a real Announce: the
            // relay's filter lets that one broadcast down the wire and
            // nothing else. The relays beacon nothing.
            mark4_Envelope announce = mark4_Envelope_init_zero;
            announce.which_body = mark4_Envelope_announce_tag;
            announce.body.announce.kind = mark4_NodeKind_GATEWAY;
            std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> beacon{};
            std::size_t beaconSize = 0U;
            if (!mark4::encodeEnvelope(announce, beacon.data(), beacon.size(), beaconSize))
            {
                return false;
            }
            m_transport.setBeacon(beacon.data(), beaconSize);
            m_relay.setRelay(true);
            m_relay.setRelayFilter(&uartFilter, nullptr);
            m_farRelay.setRelay(true);
            return m_relay.addLink(m_relayUart) && m_relay.addLink(m_relayUdp) &&
                   m_relayUdp.init() && m_relay.init() && m_farRelay.addLink(m_farUart) &&
                   m_farUdp.init() && m_farRelay.addLink(m_farUdp) && m_farRelay.init();
        }

        /// @return bytes the ESP32-like relay wrote down the wire so far
        [[nodiscard]] std::size_t wireBytesToBoard() const
        {
            return m_relayEnd.written();
        }

        /// @return relayed frames the filter kept off the wire
        [[nodiscard]] std::uint32_t filtered() const
        {
            return m_relay.filtered();
        }

        /// @brief Sends one message to the sim, by node id.
        /// @param envelope message
        /// @return true when the frame went out, false while the sim has
        ///         not been heard yet
        bool send(const mark4_Envelope &envelope)
        {
            std::array<std::uint8_t, mark4::MAX_ENVELOPE_SIZE> bytes{};
            std::size_t size = 0U;
            return mark4::encodeEnvelope(envelope, bytes.data(), bytes.size(), size) &&
                   m_transport.send(SIM_NODE, bytes.data(), size);
        }

        /// @brief Hands every pending payload to the client, the way the
        ///        hub's poll loop does.
        /// @param client client to feed
        /// @param instantUs current time [us]
        void drain(mark4::OtaClient &client, std::uint64_t instantUs)
        {
            m_pending = &client;
            m_pendingUs = instantUs;
            if (m_viaRelay)
            {
                // The far side first, so what the sim answered this step is
                // already on the wire when the near relay polls, and on the
                // ground LAN when the ground node does.
                m_farRelay.poll(instantUs, nullptr, nullptr);
                m_relay.poll(instantUs, nullptr, nullptr);
            }
            m_transport.poll(instantUs, &GroundLink::Deliver, this);
            if (m_viaRelay)
            {
                m_relay.poll(instantUs, nullptr, nullptr);
                m_farRelay.poll(instantUs, nullptr, nullptr);
            }
        }

      private:
        static void Deliver(void *context,
                            std::uint32_t src,
                            const std::uint8_t *payload,
                            std::size_t size)
        {
            static_cast<void>(src);
            auto *self = static_cast<GroundLink *>(context);
            mark4_Envelope envelope;
            if (mark4::decodeEnvelope(payload, size, envelope))
            {
                static_cast<void>(self->m_pending->onEnvelope(envelope, self->m_pendingUs));
            }
        }

        bool m_viaRelay;                                   ///< through the wire and two relays
        mark4::UdpLink m_udp;                              ///< the ground node's LAN link
        mark4::Transport m_transport{GROUND_NODE};         ///< this node
        mark4::BytePipe m_wire;                            ///< the UART between the relays
        mark4::PipeEnd m_relayEnd{m_wire.toA, m_wire.toB}; ///< ESP32 side of the wire
        mark4::PipeEnd m_farEnd{m_wire.toB, m_wire.toA};   ///< board side of the wire
        mark4::UartLink m_relayUart{m_relayEnd};           ///< the ESP32's UART link (index 0)
        mark4::UdpLink m_relayUdp;                         ///< the ESP32's LAN link, the
                                                           ///< ground's discovery port
        mark4::Transport m_relay{RELAY_NODE};              ///< the ESP32: relay + filter
        mark4::UartLink m_farUart{m_farEnd};               ///< far relay's UART link
        mark4::UdpLink m_farUdp;                           ///< far relay's link to the sim
        mark4::Transport m_farRelay{FAR_RELAY_NODE};       ///< the board's side of the wire
        mark4::OtaClient *m_pending = nullptr;             ///< client being fed by drain()
        std::uint64_t m_pendingUs = 0U;                    ///< instant handed to it
    };

    /// @return a UDP port nothing holds right now
    std::uint16_t pickFreePort()
    {
        const int socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(socketFd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = 0U;
        REQUIRE(::bind(socketFd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
                0);
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        REQUIRE(::getsockname(socketFd, reinterpret_cast<sockaddr *>(&bound), &length) == 0);
        const std::uint16_t port = ntohs(bound.sin_port);
        static_cast<void>(::close(socketFd));
        return port;
    }

    /// A running drone_sim process, stopped by the destructor whatever the
    /// test does. Its console goes to a file in the run directory: when this
    /// test fails, what the sim's fake bootloader decided is the first thing
    /// worth reading.
    class SimProcess
    {
      public:
        SimProcess() = default;

        SimProcess(const SimProcess &) = delete;
        SimProcess &operator=(const SimProcess &) = delete;
        SimProcess(SimProcess &&) = delete;
        SimProcess &operator=(SimProcess &&) = delete;

        ~SimProcess()
        {
            stop();
        }

        /// @brief Spawns drone_sim on the given ports and flash directory.
        /// @param runDirectory directory the process runs in, so what it
        ///        writes lands in the test's own scratch space instead of
        ///        wherever ctest was started from
        /// @param otaDirectory emulated flash directory
        /// @param discoveryPort shared transport port of the test
        /// @param nodeId transport identity the sim takes
        /// @return true when the process started
        bool start(const std::filesystem::path &runDirectory,
                   const std::string &otaDirectory,
                   std::uint16_t discoveryPort,
                   std::uint32_t nodeId)
        {
            const std::string discoveryPortText = std::to_string(discoveryPort);
            const std::string nodeIdText = std::to_string(nodeId);
            const std::string consolePath = (runDirectory / "drone_sim.log").string();
            const std::string runPath = runDirectory.string();
            std::array<const char *, 8> argv = {DRONE_SIM_BINARY,
                                                "--discovery-port",
                                                discoveryPortText.c_str(),
                                                "--node-id",
                                                nodeIdText.c_str(),
                                                "--ota-dir",
                                                otaDirectory.c_str(),
                                                nullptr};

            posix_spawn_file_actions_t actions;
            static_cast<void>(::posix_spawn_file_actions_init(&actions));
            // The sim writes under its working directory, so the working
            // directory is the test's, not the one ctest happens to run in.
            static_cast<void>(::posix_spawn_file_actions_addchdir_np(&actions, runPath.c_str()));
            static_cast<void>(::posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, consolePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644));
            static_cast<void>(
                ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO));
            const int status = ::posix_spawn(&m_pid,
                                             DRONE_SIM_BINARY,
                                             &actions,
                                             nullptr,
                                             const_cast<char *const *>(argv.data()),
                                             nullptr);
            static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
            if (status != 0)
            {
                m_pid = -1;
                return false;
            }
            return true;
        }

        /// @brief Stops the process and reaps it. Idempotent.
        void stop()
        {
            if (m_pid <= 0)
            {
                return;
            }
            static_cast<void>(::kill(m_pid, SIGTERM));
            int status = 0;
            static_cast<void>(::waitpid(m_pid, &status, 0));
            m_pid = -1;
        }

        /// @return true while the process has not exited on its own
        [[nodiscard]] bool alive() const
        {
            if (m_pid <= 0)
            {
                return false;
            }
            int status = 0;
            return ::waitpid(m_pid, &status, WNOHANG) == 0;
        }

      private:
        pid_t m_pid = -1; ///< -1 when nothing is running
    };

    /// @brief Runs the client the way the hub's poll loop does - tick, then
    ///        drain whatever came back - until a condition holds.
    /// @param client client to drive
    /// @param link ground side of the link
    /// @param condition what the test is waiting for
    /// @param budgetMs how long it may take [ms]
    /// @return true when the condition held before the budget ran out
    template <typename Condition>
    bool driveUntil(mark4::OtaClient &client,
                    GroundLink &link,
                    Condition condition,
                    std::uint64_t budgetMs = STEP_BUDGET_MS)
    {
        const std::uint64_t deadlineUs = nowUs() + (budgetMs * US_PER_MS);
        for (;;)
        {
            const std::uint64_t instantUs = nowUs();
            client.tick(instantUs);
            link.drain(client, instantUs);
            if (condition())
            {
                return true;
            }
            if (instantUs >= deadlineUs)
            {
                return false;
            }
            sleepStep();
        }
    }

    /// How often the initial status request is repeated while the sim is
    /// still coming up [us].
    constexpr std::uint64_t POKE_PERIOD_US = 200000U;

    /// @brief Asks the board to describe itself until an answer comes back,
    ///        repeating the request: a status request outside a session is
    ///        not retried by the client (a session's QUERY phase is), and the
    ///        very first one can land before the process has bound its
    ///        uplink. Also the way a verdict is checked against fresh
    ///        metadata: the snapshot a finished session leaves behind is the
    ///        one it last polled, which is older than its own last act.
    /// @param client client to drive
    /// @param link ground side of the link
    /// @return true once a status packet newer than the current snapshot
    ///         came back
    bool refreshBoard(mark4::OtaClient &client, GroundLink &link)
    {
        const std::uint64_t knownUs = client.board().seenAtUs;
        std::uint64_t nextPokeUs = 0U;
        return driveUntil(client, link, [&client, knownUs, &nextPokeUs] {
            if (client.board().seen && client.board().seenAtUs != knownUs)
            {
                return true;
            }
            const std::uint64_t instantUs = nowUs();
            if (instantUs >= nextPokeUs)
            {
                nextPokeUs = instantUs + POKE_PERIOD_US;
                std::string ignored;
                static_cast<void>(client.requestBoardStatus(instantUs, ignored));
            }
            return false;
        });
    }

    /// @brief The settings this test runs the client with: the real state
    ///        machine, its real timeouts shortened to what a loopback link
    ///        and a 500 ms sim wakeup actually need.
    /// @return the configuration
    mark4::OtaClient::Config testConfig()
    {
        mark4::OtaClient::Config config;
        config.statusPeriodMs = 200U;
        config.statusTries = 60U;
        config.rebootSettleMs = 500U;
        config.boardReturnTimeoutMs = 30000U;
        config.chunkAckTimeoutMs = 3000U;
        config.ackTimeoutMs = 4000U;
        // No pacing: the UART this delay protects is not in the path.
        config.chunkDelayUs = 0U;
        return config;
    }

    /// @brief The reboot command the trial boot needs.
    /// @return the message
    mark4_Envelope rebootCommand()
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_reboot_tag;
        return envelope;
    }
} // namespace

TEST_CASE("a hub-driven update of a live drone_sim confirms, then an unconfirmed one rolls back",
          "[ota][e2e]")
{
    // One run directory per test run: the emulated flash, the two bundles and
    // the sim's console all live in it, and it is removed on the way out.
    std::error_code error;
    const std::filesystem::path runDirectory = std::filesystem::temp_directory_path(error) /
                                               ("mark4_ota_e2e_" + std::to_string(::getpid()));
    std::filesystem::remove_all(runDirectory, error);
    REQUIRE(std::filesystem::create_directories(runDirectory, error));

    const std::string otaDirectory = (runDirectory / "flash").string();
    const std::uint32_t firstBuild = 0x66E00001U;
    const std::uint32_t secondBuild = 0x66E00002U;
    const std::string firstBundle = writeBundle(runDirectory / "first.ota", firstBuild, "aaaaaaaa");
    // The second bundle carries the sim's "notalive" marker: an image that
    // boots and talks but never reaches the checkpoint where it would
    // confirm itself, which is what makes the rollback observable.
    const std::string secondBundle =
        writeBundle(runDirectory / "second.ota", secondBuild, "bbbbbbbb", true);

    const std::uint16_t discoveryPort = pickFreePort();
    GroundLink link(discoveryPort);
    REQUIRE(link.open());

    SimProcess sim;
    REQUIRE(sim.start(runDirectory, otaDirectory, discoveryPort, SIM_NODE));

    mark4::OtaClient client(testConfig());
    client.setSink([&link](const mark4_Envelope &envelope, std::string &errorOut) {
        if (!link.send(envelope))
        {
            errorOut = "the loopback link refused the datagram";
            return false;
        }
        return true;
    });

    // The sim starts from an empty emulated flash: slot A runs, and it holds
    // no image header at all, which is how a desktop process says "what runs
    // here is my own build".
    REQUIRE(refreshBoard(client, link));
    REQUIRE(client.board().mcu == mark4_Mcu_SIM);
    REQUIRE(client.board().runningSlot == mark4::OTA_SLOT_A);
    REQUIRE(client.board().gitHash.empty());

    // --- The happy path: transfer, trial boot, self-confirm. ---
    std::string startError;
    REQUIRE(client.start(firstBundle, nowUs(), startError));
    REQUIRE(driveUntil(
        client, link, [&client] { return client.verdict() != mark4::OtaVerdict::NONE; }));
    INFO("phase " << mark4::otaPhaseName(client.phase()) << ", error '" << client.lastError()
                  << "'");
    REQUIRE(client.verdict() == mark4::OtaVerdict::CONFIRMED);
    REQUIRE(client.phase() == mark4::OtaPhase::CONFIRMED);
    REQUIRE(client.progress().ackedBytes == IMAGE_SIZE);

    // The sim now runs the image the bundle carried, out of the slot it was
    // never running from, and that slot is the one the metadata prefers. Read
    // from a fresh status: the confirmation is the session's last act, so the
    // snapshot it ends on predates its own record.
    REQUIRE(refreshBoard(client, link));
    REQUIRE(client.board().runningSlot == mark4::OTA_SLOT_B);
    REQUIRE(client.board().activeSlot == mark4::OTA_SLOT_B);
    REQUIRE(client.board().buildEpoch == firstBuild);
    REQUIRE(client.board().gitHash == "aaaaaaaa");
    REQUIRE(client.board().slots[mark4::OTA_SLOT_B].state == mark4_OtaSlotState_VALID);
    REQUIRE(sim.alive());

    // --- The rollback path: a trial that never confirms itself, then a
    // reset. ---
    mark4::OtaClient manual(testConfig());
    manual.setSink([&link](const mark4_Envelope &envelope, std::string &errorOut) {
        if (!link.send(envelope))
        {
            errorOut = "the loopback link refused the datagram";
            return false;
        }
        return true;
    });

    REQUIRE(manual.start(secondBundle, nowUs(), startError));
    // The broken image answers every request but never vouches for itself:
    // the hub reaches TESTING and stays there, sending nothing.
    REQUIRE(
        driveUntil(manual, link, [&manual] { return manual.phase() == mark4::OtaPhase::TESTING; }));
    REQUIRE(manual.phase() == mark4::OtaPhase::TESTING);
    REQUIRE(manual.board().runningSlot == mark4::OTA_SLOT_A);
    REQUIRE(manual.board().buildEpoch == secondBuild);
    REQUIRE(manual.board().gitHash == "bbbbbbbb");
    REQUIRE(manual.board().slots[mark4::OTA_SLOT_A].state == mark4_OtaSlotState_TESTING);
    // The metadata still prefers the confirmed image: a trial boot never
    // moves the active slot, which is exactly what makes the rollback free.
    REQUIRE(manual.board().activeSlot == mark4::OTA_SLOT_B);

    // The reset the trial does not survive. On a board this is a watchdog, a
    // crash or a power cycle; here it is the same reboot command the hub
    // sends, and the sim's fake bootloader takes it from there.
    REQUIRE(link.send(rebootCommand()));

    REQUIRE(driveUntil(
        manual, link, [&manual] { return manual.verdict() != mark4::OtaVerdict::NONE; }));
    INFO("phase " << mark4::otaPhaseName(manual.phase()) << ", error '" << manual.lastError()
                  << "'");
    REQUIRE(manual.verdict() == mark4::OtaVerdict::ROLLED_BACK);
    REQUIRE(manual.phase() == mark4::OtaPhase::ROLLED_BACK);

    // Back on the image the first update installed, with the slot that was on
    // trial marked bad so the bootloader never tries it again.
    REQUIRE(refreshBoard(manual, link));
    REQUIRE(manual.board().runningSlot == mark4::OTA_SLOT_B);
    REQUIRE(manual.board().activeSlot == mark4::OTA_SLOT_B);
    REQUIRE(manual.board().buildEpoch == firstBuild);
    REQUIRE(manual.board().gitHash == "aaaaaaaa");
    REQUIRE(manual.board().slots[mark4::OTA_SLOT_A].state == mark4_OtaSlotState_BAD);
    REQUIRE(manual.board().slots[mark4::OTA_SLOT_B].state == mark4_OtaSlotState_VALID);
    REQUIRE(sim.alive());

    sim.stop();
    std::filesystem::remove_all(runDirectory, error);
}

TEST_CASE("a hub-driven update crosses the esp32 relay, its filter and the serial framing",
          "[ota][e2e]")
{
    // The happy path of the test above, with the ground node reaching the sim
    // through the ESP32-like relay, its UART filter, the serial framing and
    // a far relay standing for the board's transport. The transfer, its
    // acknowledgements, the reboot and the self-confirmation all have to
    // cross the framing both ways.
    std::error_code error;
    const std::filesystem::path runDirectory =
        std::filesystem::temp_directory_path(error) /
        ("mark4_ota_e2e_relay_" + std::to_string(::getpid()));
    std::filesystem::remove_all(runDirectory, error);
    REQUIRE(std::filesystem::create_directories(runDirectory, error));

    const std::string otaDirectory = (runDirectory / "flash").string();
    const std::uint32_t build = 0x66E00003U;
    const std::string bundle = writeBundle(runDirectory / "relayed.ota", build, "cccccccc");

    const std::uint16_t discoveryPort = pickFreePort();
    const std::uint16_t farPort = pickFreePort();
    GroundLink link(discoveryPort, farPort);
    REQUIRE(link.open());

    // The sim lives on the far LAN, behind the wire: where the board is.
    SimProcess sim;
    REQUIRE(sim.start(runDirectory, otaDirectory, farPort, SIM_NODE));

    mark4::OtaClient client(testConfig());
    client.setSink([&link](const mark4_Envelope &envelope, std::string &errorOut) {
        if (!link.send(envelope))
        {
            errorOut = "the relayed link refused the frame";
            return false;
        }
        return true;
    });

    REQUIRE(refreshBoard(client, link));
    REQUIRE(client.board().mcu == mark4_Mcu_SIM);
    REQUIRE(client.board().runningSlot == mark4::OTA_SLOT_A);

    std::string startError;
    REQUIRE(client.start(bundle, nowUs(), startError));
    REQUIRE(driveUntil(
        client, link, [&client] { return client.verdict() != mark4::OtaVerdict::NONE; }));
    INFO("phase " << mark4::otaPhaseName(client.phase()) << ", error '" << client.lastError()
                  << "'");
    REQUIRE(client.verdict() == mark4::OtaVerdict::CONFIRMED);
    REQUIRE(client.progress().ackedBytes == IMAGE_SIZE);

    REQUIRE(refreshBoard(client, link));
    REQUIRE(client.board().runningSlot == mark4::OTA_SLOT_B);
    REQUIRE(client.board().buildEpoch == build);
    REQUIRE(client.board().gitHash == "cccccccc");
    REQUIRE(sim.alive());

    // The whole transfer went down the wire: what the ground unicast to the
    // sim plus its own beacons, and nothing the filter had to refuse (the
    // ground never broadcast anything but its Announce).
    CHECK(link.wireBytesToBoard() > IMAGE_SIZE);
    CHECK(link.filtered() == 0U);

    sim.stop();
    std::filesystem::remove_all(runDirectory, error);
}

TEST_CASE("an OtaCommand from a gateway client drives the update of the node it names",
          "[ota][e2e]")
{
    // The happy path once more, entered the way a page enters it: one
    // OtaCommand naming the node, applied by the gateway's dispatcher, the
    // state read back as the OtaState message the page paints.
    std::error_code error;
    const std::filesystem::path runDirectory = std::filesystem::temp_directory_path(error) /
                                               ("mark4_ota_e2e_cmd_" + std::to_string(::getpid()));
    std::filesystem::remove_all(runDirectory, error);
    REQUIRE(std::filesystem::create_directories(runDirectory, error));

    const std::string otaDirectory = (runDirectory / "flash").string();
    const std::uint32_t build = 0x66E00004U;
    const std::string bundle = writeBundle(runDirectory / "commanded.ota", build, "dddddddd");

    const std::uint16_t discoveryPort = pickFreePort();
    GroundLink link(discoveryPort);
    REQUIRE(link.open());
    SimProcess sim;
    REQUIRE(sim.start(runDirectory, otaDirectory, discoveryPort, SIM_NODE));

    std::uint32_t target = 0U;
    mark4::OtaClient client(testConfig());
    client.setSink([&link, &target](const mark4_Envelope &envelope, std::string &errorOut) {
        // The gateway routes every updater message to the node the command
        // named; here the link only knows the sim, so the check is the target.
        if (target != SIM_NODE || !link.send(envelope))
        {
            errorOut = "no route to the target node";
            return false;
        }
        return true;
    });
    mark4_OtaCommand poke = mark4_OtaCommand_init_zero;
    poke.op = mark4_OtaCommand_Op_STATUS_REQUEST;
    poke.target_node = SIM_NODE;
    std::string refusal;
    // The first request may leave before the sim has been heard: the
    // command still fixes the target, refreshBoard() repeats the request.
    static_cast<void>(mark4::applyOtaCommand(client, poke, target, nowUs(), refusal));
    CHECK(target == SIM_NODE);
    REQUIRE(refreshBoard(client, link));

    mark4_OtaCommand command = mark4_OtaCommand_init_zero;
    command.op = mark4_OtaCommand_Op_START;
    command.target_node = SIM_NODE;
    mark4::copyWireString(bundle, command.bundle_path, sizeof(command.bundle_path));
    REQUIRE(mark4::applyOtaCommand(client, command, target, nowUs(), refusal));
    CHECK(target == SIM_NODE);

    // While the session runs, a command aimed elsewhere is refused.
    mark4_OtaCommand elsewhere = mark4_OtaCommand_init_zero;
    elsewhere.op = mark4_OtaCommand_Op_STATUS_REQUEST;
    elsewhere.target_node = SIM_NODE + 1U;
    CHECK(!mark4::applyOtaCommand(client, elsewhere, target, nowUs(), refusal));
    CHECK(target == SIM_NODE);

    REQUIRE(driveUntil(
        client, link, [&client] { return client.verdict() != mark4::OtaVerdict::NONE; }));
    const mark4_OtaState state = mark4::otaStateOf(client, target);
    INFO("phase " << state.phase << ", error '" << state.last_error << "'");
    CHECK(state.phase == mark4_OtaState_Phase_CONFIRMED);
    CHECK(state.verdict == mark4_OtaState_Verdict_VERDICT_CONFIRMED);
    CHECK(state.target_node == SIM_NODE);
    CHECK(state.progress.acked_bytes == IMAGE_SIZE);
    CHECK(state.progress.total_bytes == IMAGE_SIZE);
    CHECK(std::string(state.bundle.git_hash) == "dddddddd");
    CHECK(sim.alive());

    sim.stop();
    std::filesystem::remove_all(runDirectory, error);
}
