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

#include "hub/ota_bundle.hpp"
#include "hub/ota_client.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"

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
    constexpr std::size_t RECEIVE_BUFFER_SIZE = 512U;

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

    /// One firmware version, as an image carries it.
    struct Version
    {
        std::uint8_t major; ///< major number
        std::uint8_t minor; ///< minor number
        std::uint8_t patch; ///< patch number
    };

    /// @brief Builds one complete image for a slot: a fully stamped
    ///        OtaImageHeader followed by a payload whose bytes depend on the
    ///        slot and the version, so no two images of this test are alike.
    /// @param slot slot the image is linked for
    /// @param version version the image announces
    /// @param gitHash eight-character build hash
    /// @return the image bytes
    std::vector<std::uint8_t> makeImage(std::uint8_t slot,
                                        const Version &version,
                                        const std::string &gitHash)
    {
        std::vector<std::uint8_t> image(IMAGE_SIZE, 0xFFU);
        for (std::uint32_t i = 0U; i < IMAGE_PAYLOAD_SIZE; ++i)
        {
            image[mark4::OTA_IMAGE_HEADER_SIZE + i] =
                static_cast<std::uint8_t>(i + slot + version.major + version.minor);
        }

        mark4::OtaImageHeader header{};
        header.magic = mark4::OTA_IMAGE_MAGIC;
        header.headerVersion = mark4::OTA_IMAGE_HEADER_VERSION;
        header.mcuId = mark4::OTA_MCU_SIM;
        header.slotId = slot;
        header.imageSize = IMAGE_SIZE;
        header.imageCrc =
            mark4::otaImageCrc32(image.data() + mark4::OTA_IMAGE_HEADER_SIZE, IMAGE_PAYLOAD_SIZE);
        header.versionMajor = version.major;
        header.versionMinor = version.minor;
        header.versionPatch = version.patch;
        header.reserved0 = 0xFFU;
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
    /// @param version version both images announce
    /// @param gitHash eight-character build hash
    /// @return the bundle path, for chaining
    std::string writeBundle(const std::filesystem::path &path,
                            const Version &version,
                            const std::string &gitHash)
    {
        const std::vector<std::uint8_t> slotA = makeImage(mark4::OTA_SLOT_A, version, gitHash);
        const std::vector<std::uint8_t> slotB = makeImage(mark4::OTA_SLOT_B, version, gitHash);

        // The manifest reads exactly like the one scripts/make_ota.py writes:
        // compact, keys sorted, every number decimal.
        std::string manifest = R"({"gitHash":")" + gitHash + R"(","images":[)";
        manifest += R"({"crc32":)" +
                    std::to_string(mark4::otaImageCrc32(slotA.data(), slotA.size())) +
                    R"(,"size":)" + std::to_string(slotA.size()) + R"(,"slot":0},)";
        manifest += R"({"crc32":)" +
                    std::to_string(mark4::otaImageCrc32(slotB.data(), slotB.size())) +
                    R"(,"size":)" + std::to_string(slotB.size()) + R"(,"slot":1}],)";
        manifest += R"("mcuId":)" + std::to_string(mark4::OTA_MCU_SIM);
        manifest +=
            R"(,"name":"drone_sim","protocolVersion":)" + std::to_string(mark4::PROTOCOL_VERSION);
        manifest += R"(,"version":{"major":)" + std::to_string(version.major) + R"(,"minor":)" +
                    std::to_string(version.minor) + R"(,"patch":)" + std::to_string(version.patch) +
                    "}}";

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

    /// The ground side's UDP end of the link: one socket, bound to an
    /// ephemeral port the sim is told to broadcast its telemetry to, and used
    /// to send commands to the sim's uplink port. Exactly the two directions
    /// the hub uses, on exactly the two ports it uses them on.
    class GroundLink
    {
      public:
        GroundLink() = default;

        GroundLink(const GroundLink &) = delete;
        GroundLink &operator=(const GroundLink &) = delete;
        GroundLink(GroundLink &&) = delete;
        GroundLink &operator=(GroundLink &&) = delete;

        ~GroundLink()
        {
            if (m_socketFd >= 0)
            {
                static_cast<void>(::close(m_socketFd));
            }
        }

        /// @brief Binds the receive port and readies the send side.
        /// @return true when the socket is usable
        bool open()
        {
            m_socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (m_socketFd < 0)
            {
                return false;
            }
            const int enable = 1;
            static_cast<void>(
                ::setsockopt(m_socketFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)));
            // The sim broadcasts its answers, like every other packet it
            // sends: the receive side must accept a datagram addressed to the
            // broadcast address, which binding INADDR_ANY does.
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = 0U;
            if (::bind(m_socketFd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) !=
                0)
            {
                return false;
            }
            sockaddr_in bound{};
            socklen_t length = sizeof(bound);
            if (::getsockname(m_socketFd, reinterpret_cast<sockaddr *>(&bound), &length) != 0)
            {
                return false;
            }
            m_port = ntohs(bound.sin_port);
            return ::fcntl(m_socketFd, F_SETFL, O_NONBLOCK) == 0;
        }

        /// @return the port the sim must broadcast to
        [[nodiscard]] std::uint16_t port() const
        {
            return m_port;
        }

        /// @brief Points the send side at the sim's command uplink.
        /// @param commandPort port the sim binds its command receiver to
        void setCommandPort(std::uint16_t commandPort)
        {
            m_commandPort = commandPort;
        }

        /// @brief Sends one packet to the sim's command uplink.
        /// @param data packet bytes
        /// @param size packet size
        /// @return true when the datagram went out
        bool send(const std::uint8_t *data, std::size_t size) const
        {
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(m_commandPort);
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return ::sendto(m_socketFd,
                            data,
                            size,
                            0,
                            reinterpret_cast<const sockaddr *>(&target),
                            sizeof(target)) == static_cast<ssize_t>(size);
        }

        /// @brief Hands every pending datagram to the client, the way the
        ///        hub's poll loop does.
        /// @param client client to feed
        /// @param instantUs current time [us]
        void drain(mark4::OtaClient &client, std::uint64_t instantUs) const
        {
            std::array<std::uint8_t, RECEIVE_BUFFER_SIZE> buffer{};
            for (;;)
            {
                const ssize_t got = ::recv(m_socketFd, buffer.data(), buffer.size(), 0);
                if (got <= 0)
                {
                    return;
                }
                static_cast<void>(
                    client.onPacket(buffer.data(), static_cast<std::size_t>(got), instantUs));
            }
        }

      private:
        int m_socketFd = -1;              ///< bound receive and send socket
        std::uint16_t m_port = 0U;        ///< port the sim broadcasts to
        std::uint16_t m_commandPort = 0U; ///< port the sim listens on
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
        /// @param runDirectory directory the process runs in, so its blackbox
        ///        file lands in the test's own scratch space instead of
        ///        wherever ctest was started from
        /// @param otaDirectory emulated flash directory
        /// @param simPort lockstep port (nothing drives it in this test)
        /// @param telemetryPort port the sim broadcasts its answers to
        /// @param commandPort port the sim binds its command uplink to
        /// @return true when the process started
        bool start(const std::filesystem::path &runDirectory,
                   const std::string &otaDirectory,
                   std::uint16_t simPort,
                   std::uint16_t telemetryPort,
                   std::uint16_t commandPort)
        {
            const std::string simPortText = std::to_string(simPort);
            const std::string telemetryPortText = std::to_string(telemetryPort);
            const std::string commandPortText = std::to_string(commandPort);
            const std::string consolePath = (runDirectory / "drone_sim.log").string();
            const std::string runPath = runDirectory.string();
            std::array<const char *, 10> argv = {DRONE_SIM_BINARY,
                                                 "--sim-port",
                                                 simPortText.c_str(),
                                                 "--telemetry-port",
                                                 telemetryPortText.c_str(),
                                                 "--rc-port",
                                                 commandPortText.c_str(),
                                                 "--ota-dir",
                                                 otaDirectory.c_str(),
                                                 nullptr};

            posix_spawn_file_actions_t actions;
            static_cast<void>(::posix_spawn_file_actions_init(&actions));
            // The sim writes its blackbox under the working directory, so the
            // working directory is the test's, not the one ctest happens to
            // run in.
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
    /// @param autoConfirm true to confirm without an operator
    /// @return the configuration
    mark4::OtaClient::Config testConfig(bool autoConfirm)
    {
        mark4::OtaClient::Config config;
        config.statusPeriodMs = 200U;
        config.statusTries = 60U;
        config.rebootSettleMs = 500U;
        config.boardReturnTimeoutMs = 30000U;
        config.healthyLinkMs = 400U;
        config.healthyStatuses = 2U;
        config.chunkAckTimeoutMs = 3000U;
        config.ackTimeoutMs = 4000U;
        // No pacing: the bridge UART this delay protects is not in the path.
        config.chunkDelayUs = 0U;
        config.autoConfirm = autoConfirm;
        return config;
    }

    /// @brief The reboot command the trial boot needs, on the wire.
    /// @return the three bytes
    std::array<std::uint8_t, mark4::REBOOT_COMMAND_PACKET_SIZE> rebootCommand()
    {
        return {mark4::PROTOCOL_VERSION,
                static_cast<std::uint8_t>(mark4::PacketType::REBOOT_COMMAND),
                mark4::BOARD_REBOOT_MAGIC};
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
    const Version firstVersion = {1U, 2U, 3U};
    const Version secondVersion = {4U, 5U, 6U};
    const std::string firstBundle =
        writeBundle(runDirectory / "first.ota", firstVersion, "aaaaaaaa");
    const std::string secondBundle =
        writeBundle(runDirectory / "second.ota", secondVersion, "bbbbbbbb");

    GroundLink link;
    REQUIRE(link.open());
    const std::uint16_t commandPort = pickFreePort();
    const std::uint16_t simPort = pickFreePort();
    link.setCommandPort(commandPort);

    SimProcess sim;
    REQUIRE(sim.start(runDirectory, otaDirectory, simPort, link.port(), commandPort));

    mark4::OtaClient client(testConfig(true));
    client.setSink([&link](const std::uint8_t *data, std::size_t size, std::string &errorOut) {
        if (!link.send(data, size))
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
    REQUIRE(client.board().mcuId == mark4::OTA_MCU_SIM);
    REQUIRE(client.board().runningSlot == mark4::OTA_SLOT_A);
    REQUIRE(client.board().gitHash.empty());

    // --- The happy path: transfer, trial boot, auto-confirm. ---
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
    REQUIRE(client.board().versionMajor == firstVersion.major);
    REQUIRE(client.board().versionMinor == firstVersion.minor);
    REQUIRE(client.board().versionPatch == firstVersion.patch);
    REQUIRE(client.board().gitHash == "aaaaaaaa");
    REQUIRE(client.board().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    REQUIRE(sim.alive());

    // --- The rollback path: a trial nobody confirms, then a reset. ---
    mark4::OtaClient manual(testConfig(false));
    manual.setSink([&link](const std::uint8_t *data, std::size_t size, std::string &errorOut) {
        if (!link.send(data, size))
        {
            errorOut = "the loopback link refused the datagram";
            return false;
        }
        return true;
    });

    REQUIRE(manual.start(secondBundle, nowUs(), startError));
    // Manual confirm: the client stops at TESTING and waits for a gesture
    // that never comes.
    REQUIRE(driveUntil(manual, link, [&manual] {
        return manual.phase() == mark4::OtaPhase::TESTING || manual.confirmReady();
    }));
    REQUIRE(manual.phase() == mark4::OtaPhase::TESTING);
    REQUIRE(manual.board().runningSlot == mark4::OTA_SLOT_A);
    REQUIRE(manual.board().versionMajor == secondVersion.major);
    REQUIRE(manual.board().gitHash == "bbbbbbbb");
    REQUIRE(manual.board().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_TESTING);
    // The metadata still prefers the confirmed image: a trial boot never
    // moves the active slot, which is exactly what makes the rollback free.
    REQUIRE(manual.board().activeSlot == mark4::OTA_SLOT_B);

    // The reset the trial does not survive. On a board this is a watchdog, a
    // crash or a power cycle; here it is the same reboot command the hub
    // sends, and the sim's fake bootloader takes it from there.
    const auto reboot = rebootCommand();
    REQUIRE(link.send(reboot.data(), reboot.size()));

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
    REQUIRE(manual.board().versionMajor == firstVersion.major);
    REQUIRE(manual.board().versionMinor == firstVersion.minor);
    REQUIRE(manual.board().versionPatch == firstVersion.patch);
    REQUIRE(manual.board().gitHash == "aaaaaaaa");
    REQUIRE(manual.board().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_BAD);
    REQUIRE(manual.board().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    REQUIRE(sim.alive());

    sim.stop();
    std::filesystem::remove_all(runDirectory, error);
}
