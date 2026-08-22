/// @file
/// @brief The ground side of the firmware update: what the bundle reader
///        accepts and refuses, and what the session state machine does when
///        driven by a scripted board - the happy path, a lost chunk
///        acknowledgement, an erase that never finishes, a CRC refusal, a
///        board that refuses while armed, and the rollback verdict.
///
/// The client owns no socket and no clock: packets go into onPacket() and
/// time into tick(), so the fake board below is the whole test rig.
///
/// CHECK(!x) rather than CHECK_FALSE: that flag combination trips
/// clang-analyzer's enum-cast check inside Catch2's own headers.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "hub/json_codec.hpp"
#include "hub/ota_bundle.hpp"
#include "hub/ota_client.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"

namespace
{
    using namespace mark4;

    constexpr std::uint64_t US_PER_MS = 1000U;

    /// Payload after the header of every test image; two windows and a bit,
    /// so the go-back-N cases have somewhere to go back to.
    constexpr std::uint32_t TEST_PAYLOAD = 8000U;

    /// What a slot holds on the fake board.
    constexpr std::uint32_t TEST_SLOT_SIZE = 131072U;

    /// Identities the scripted board reports before and after the update.
    constexpr const char *OLD_HASH = "aaaaaaaa";
    constexpr const char *NEW_HASH = "bbbbbbbb";

    /// A directory of its own per test, removed on the way out: a bundle is
    /// a file, so exercising the reader means touching a real filesystem.
    class ScratchDirectory
    {
      public:
        ScratchDirectory()
        {
            std::error_code code;
            m_path =
                std::filesystem::temp_directory_path(code) /
                ("mark4_ota_" + std::to_string(::getpid()) + "_" + std::to_string(s_counter++));
            static_cast<void>(std::filesystem::create_directories(m_path, code));
        }

        ScratchDirectory(const ScratchDirectory &) = delete;
        ScratchDirectory &operator=(const ScratchDirectory &) = delete;
        ScratchDirectory(ScratchDirectory &&) = delete;
        ScratchDirectory &operator=(ScratchDirectory &&) = delete;

        ~ScratchDirectory()
        {
            std::error_code code;
            static_cast<void>(std::filesystem::remove_all(m_path, code));
        }

        /// @param fileName name of the file inside the directory
        /// @return its full path
        [[nodiscard]] std::string pathOf(const std::string &fileName) const
        {
            return (m_path / fileName).string();
        }

        /// @brief Writes raw bytes into the directory.
        /// @param fileName name of the file
        /// @param bytes content
        /// @return the path written
        [[nodiscard]] std::string write(const std::string &fileName,
                                        const std::vector<std::uint8_t> &bytes) const
        {
            const std::string path = pathOf(fileName);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            return path;
        }

      private:
        std::filesystem::path m_path;
        static int s_counter;
    };

    int ScratchDirectory::s_counter = 0;

    /// @brief Appends a little-endian u32 to a byte buffer.
    /// @param bytes buffer to append to
    /// @param value value to append
    void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
    {
        static constexpr std::uint32_t BYTE_MASK = 0xFFU;
        static constexpr std::uint32_t BYTE_BITS = 8U;
        for (std::uint32_t shift = 0U; shift < 4U; ++shift)
        {
            bytes.push_back(static_cast<std::uint8_t>((value >> (shift * BYTE_BITS)) & BYTE_MASK));
        }
    }

    /// @brief Copies a git hash into a fixed wire field, zero padded.
    /// @param text hash text, at most OTA_GIT_HASH_SIZE characters
    /// @return the field
    std::array<char, OTA_GIT_HASH_SIZE> hashField(const std::string &text)
    {
        std::array<char, OTA_GIT_HASH_SIZE> field{};
        const std::size_t length = std::min(text.size(), field.size());
        std::memcpy(field.data(), text.data(), length);
        return field;
    }

    /// @brief Builds one complete firmware image: the stamped header, then a
    ///        recognizable payload.
    /// @param slot slot the image is linked for
    /// @param mcuId chip the image is built for
    /// @param version version triplet, all three the same number
    /// @param gitHash short hash to stamp
    /// @return the image bytes
    std::vector<std::uint8_t> makeImage(std::uint8_t slot,
                                        std::uint8_t mcuId,
                                        std::uint8_t version,
                                        const std::string &gitHash)
    {
        std::vector<std::uint8_t> image(OTA_IMAGE_HEADER_SIZE + TEST_PAYLOAD, 0xFFU);
        OtaImageHeader header{};
        std::memset(&header, 0xFF, sizeof(header));
        header.magic = OTA_IMAGE_MAGIC;
        header.headerVersion = OTA_IMAGE_HEADER_VERSION;
        header.mcuId = mcuId;
        header.slotId = slot;
        header.imageSize = static_cast<std::uint32_t>(image.size());
        header.imageCrc = otaImageCrc32(image.data() + OTA_IMAGE_HEADER_SIZE, TEST_PAYLOAD);
        header.versionMajor = version;
        header.versionMinor = version;
        header.versionPatch = version;
        const auto field = hashField(gitHash);
        std::memcpy(&header.gitHash, field.data(), field.size());
        std::memcpy(image.data(), &header, sizeof(header));
        for (std::uint32_t index = 0U; index < TEST_PAYLOAD; ++index)
        {
            // A byte pattern that depends on the offset, so a chunk written
            // at the wrong offset would not go unnoticed.
            image[OTA_IMAGE_HEADER_SIZE + index] =
                static_cast<std::uint8_t>((index * 7U + slot) & 0xFFU);
        }
        return image;
    }

    /// One assembled bundle file, plus the facts a test wants to assert on.
    struct BuiltBundle
    {
        std::vector<std::uint8_t> bytes;                 ///< the file content
        std::array<std::vector<std::uint8_t>, 2> images; ///< image bytes per slot
        std::array<std::uint32_t, 2> crc{};              ///< announced CRC per slot
    };

    /// @brief Assembles a bundle file the way scripts/make_ota.py does.
    /// @param mcuId chip the build targets
    /// @param version version triplet number
    /// @param gitHash short hash of the build
    /// @param protocolVersion wire version to announce
    /// @param breakCrcOfSlot slot whose announced CRC is deliberately wrong,
    ///        or 2 to keep both honest
    /// @return the file and its facts
    BuiltBundle buildBundle(std::uint8_t mcuId,
                            std::uint8_t version,
                            const std::string &gitHash,
                            std::uint8_t protocolVersion,
                            std::uint8_t breakCrcOfSlot = 2U)
    {
        BuiltBundle built;
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            built.images[slot] = makeImage(slot, mcuId, version, gitHash);
            built.crc[slot] = otaImageCrc32(built.images[slot].data(), built.images[slot].size());
            if (slot == breakCrcOfSlot)
            {
                built.crc[slot] ^= 1U;
            }
        }
        std::string manifest = R"({"name":"drone_firmware","mcuId":)";
        manifest += std::to_string(mcuId);
        manifest += R"(,"version":{"major":)" + std::to_string(version) + R"(,"minor":)" +
                    std::to_string(version) + R"(,"patch":)" + std::to_string(version) + "}";
        manifest += R"(,"gitHash":")" + gitHash + R"(","protocolVersion":)" +
                    std::to_string(protocolVersion) + R"(,"images":[)";
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            manifest += slot == 0U ? "" : ",";
            manifest += R"({"slot":)" + std::to_string(slot) + R"(,"size":)" +
                        std::to_string(built.images[slot].size()) + R"(,"crc32":)" +
                        std::to_string(built.crc[slot]) + "}";
        }
        manifest += "]}";

        const char *magic = otaBundleMagic();
        built.bytes.assign(magic, magic + OTA_BUNDLE_MAGIC_SIZE);
        appendU32(built.bytes, static_cast<std::uint32_t>(manifest.size()));
        built.bytes.insert(built.bytes.end(), manifest.begin(), manifest.end());
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            appendU32(built.bytes, static_cast<std::uint32_t>(built.images[slot].size()));
            built.bytes.insert(
                built.bytes.end(), built.images[slot].begin(), built.images[slot].end());
        }
        return built;
    }

    /// @brief Builds one OtaStatusPacket the way a board would send it.
    /// @param runningSlot slot the reported firmware runs from
    /// @param runningState state of that slot
    /// @param version version triplet number
    /// @param gitHash short hash
    /// @param busy true to report an open transfer session
    /// @return the packet bytes
    std::vector<std::uint8_t> statusPacket(std::uint8_t runningSlot,
                                           std::uint8_t runningState,
                                           std::uint8_t version,
                                           const std::string &gitHash,
                                           bool busy = false)
    {
        OtaStatusPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_STATUS);
        packet.mcuId = OTA_MCU_STM32F405;
        packet.runningSlot = runningSlot;
        // During a trial boot the metadata still prefers the other slot;
        // everywhere else active and running coincide.
        packet.activeSlot = (runningState == OTA_SLOT_TESTING)
                                ? static_cast<std::uint8_t>(1U - runningSlot)
                                : runningSlot;
        std::array<std::uint8_t, OTA_SLOT_COUNT> states = {OTA_SLOT_EMPTY, OTA_SLOT_EMPTY};
        states[runningSlot] = runningState;
        std::memcpy(&packet.slotState, states.data(), states.size());
        packet.updaterBusy = busy ? 1U : 0U;
        packet.versionMajor = version;
        packet.versionMinor = version;
        packet.versionPatch = version;
        const auto field = hashField(gitHash);
        std::memcpy(&packet.gitHash, field.data(), field.size());
        packet.slotSize = TEST_SLOT_SIZE;
        packet.maxChunkData = static_cast<std::uint16_t>(OTA_CHUNK_DATA_SIZE);
        std::vector<std::uint8_t> bytes(sizeof(packet));
        std::memcpy(bytes.data(), &packet, sizeof(packet));
        return bytes;
    }

    /// @brief Builds one OtaAckPacket.
    /// @param session session nonce to echo
    /// @param acked packet type being answered
    /// @param result one of the OTA_RESULT_* values
    /// @return the packet bytes
    std::vector<std::uint8_t> ackPacket(std::uint32_t session,
                                        PacketType acked,
                                        std::uint8_t result)
    {
        OtaAckPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_ACK);
        packet.session = session;
        packet.ackedType = static_cast<std::uint8_t>(acked);
        packet.result = result;
        std::vector<std::uint8_t> bytes(sizeof(packet));
        std::memcpy(bytes.data(), &packet, sizeof(packet));
        return bytes;
    }

    /// @brief Builds one OtaChunkAckPacket.
    /// @param session session nonce to echo
    /// @param nextOffset first image byte still missing
    /// @return the packet bytes
    std::vector<std::uint8_t> chunkAckPacket(std::uint32_t session, std::uint32_t nextOffset)
    {
        OtaChunkAckPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_CHUNK_ACK);
        packet.session = session;
        packet.nextOffset = nextOffset;
        std::vector<std::uint8_t> bytes(sizeof(packet));
        std::memcpy(bytes.data(), &packet, sizeof(packet));
        return bytes;
    }

    /// The board the client talks to: it records what went out and answers
    /// exactly what a test tells it to.
    class Bench
    {
      public:
        Bench()
        {
            OtaClient::Config config;
            // The pacing delay is the one production default a test must
            // drop: it exists to spread a window over milliseconds, and the
            // window is what is under test, not the milliseconds.
            config.chunkDelayUs = 0U;
            m_client = OtaClient(config);
            m_client.setSink(
                [this](const std::uint8_t *data, std::size_t size, std::string &errorOut) {
                    if (!m_reachable)
                    {
                        errorOut = "no serial link to the board";
                        return false;
                    }
                    m_sent.emplace_back(data, data + size);
                    return true;
                });
        }

        /// @return the client under test
        OtaClient &client()
        {
            return m_client;
        }

        /// @return the current test instant [us]
        [[nodiscard]] std::uint64_t nowUs() const
        {
            return m_nowUs;
        }

        /// @brief Moves time forward and runs one tick.
        /// @param milliseconds how far to move
        void advance(std::uint64_t milliseconds)
        {
            m_nowUs += milliseconds * US_PER_MS;
            m_client.tick(m_nowUs);
        }

        /// @brief Hands one packet to the client at the current instant.
        /// @param bytes packet bytes
        void feed(const std::vector<std::uint8_t> &bytes)
        {
            REQUIRE(m_client.onPacket(bytes.data(), bytes.size(), m_nowUs));
        }

        /// @brief Cuts or restores the route to the board.
        /// @param on true when the board is reachable
        void setReachable(bool on)
        {
            m_reachable = on;
        }

        /// @param type packet type to count
        /// @return how many packets of that type went out
        [[nodiscard]] std::size_t countSent(PacketType type) const
        {
            std::size_t count = 0U;
            for (const auto &packet : m_sent)
            {
                if (hasHeader(packet.data(), packet.size(), type))
                {
                    ++count;
                }
            }
            return count;
        }

        /// @param type packet type to look for
        /// @return the last packet of that type, empty when none went out
        [[nodiscard]] std::vector<std::uint8_t> lastSent(PacketType type) const
        {
            std::vector<std::uint8_t> found;
            for (const auto &entry : m_sent)
            {
                if (hasHeader(entry.data(), entry.size(), type))
                {
                    found = entry;
                }
            }
            return found;
        }

        /// @return the session nonce of the OTA_BEGIN that went out
        [[nodiscard]] std::uint32_t session() const
        {
            const auto begin = lastSent(PacketType::OTA_BEGIN);
            REQUIRE(begin.size() == OTA_BEGIN_PACKET_SIZE);
            OtaBeginPacket packet{};
            std::memcpy(&packet, begin.data(), sizeof(packet));
            return packet.session;
        }

        /// @return the offsets of every OTA_CHUNK that went out, in order
        [[nodiscard]] std::vector<std::uint32_t> chunkOffsets() const
        {
            std::vector<std::uint32_t> offsets;
            for (const auto &entry : m_sent)
            {
                if (hasHeader(entry.data(), entry.size(), PacketType::OTA_CHUNK))
                {
                    OtaChunkPacket packet{};
                    std::memcpy(&packet, entry.data(), sizeof(packet));
                    // Copy the packed field out before push_back binds a
                    // reference to it; the member itself is misaligned.
                    const std::uint32_t offset = packet.offset;
                    offsets.push_back(offset);
                }
            }
            return offsets;
        }

        /// @brief Forgets what went out, so the next assertion counts only
        ///        what the step under test produced.
        void clearSent()
        {
            m_sent.clear();
        }

      private:
        OtaClient m_client;
        std::vector<std::vector<std::uint8_t>> m_sent;
        std::uint64_t m_nowUs = 1'000'000U;
        bool m_reachable = true;
    };

    /// @brief Writes a good bundle and starts a session against a board
    ///        running the old firmware from slot A.
    /// @param bench bench to drive
    /// @param directory where to write the bundle
    /// @return the total bytes of the image that will be sent
    std::uint32_t startHappySession(Bench &bench, const ScratchDirectory &directory)
    {
        const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, PROTOCOL_VERSION);
        const std::string path = directory.write("drone_firmware.ota", built.bytes);
        std::string error;
        REQUIRE(bench.client().start(path, bench.nowUs(), error));
        REQUIRE(bench.client().phase() == OtaPhase::QUERY);
        bench.feed(statusPacket(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
        REQUIRE(bench.client().phase() == OtaPhase::ERASING);
        REQUIRE(bench.client().targetSlot() == OTA_SLOT_B);
        return static_cast<std::uint32_t>(built.images[OTA_SLOT_B].size());
    }

    /// @brief Runs the whole transfer, acknowledging every window.
    /// @param bench bench to drive
    /// @param totalBytes image bytes being transferred
    void runTransfer(Bench &bench, std::uint32_t totalBytes)
    {
        const std::uint32_t session = bench.session();
        bench.feed(ackPacket(session, PacketType::OTA_BEGIN, OTA_RESULT_OK));
        REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);
        while (bench.client().phase() == OtaPhase::TRANSFER)
        {
            const std::uint32_t sent = bench.client().progress().sentBytes;
            REQUIRE(sent > bench.client().progress().ackedBytes);
            bench.feed(chunkAckPacket(session, sent));
        }
        REQUIRE(bench.client().phase() == OtaPhase::VERIFYING);
        REQUIRE(bench.client().progress().ackedBytes == totalBytes);
    }

    /// @brief Takes the board through the trial boot up to TESTING.
    /// @param bench bench to drive
    void runTrialBoot(Bench &bench)
    {
        const std::uint32_t session = bench.session();
        bench.feed(ackPacket(session, PacketType::OTA_FINISH, OTA_RESULT_OK));
        REQUIRE(bench.client().phase() == OtaPhase::REBOOTING);
        REQUIRE(bench.countSent(PacketType::REBOOT_COMMAND) == 1U);
        bench.advance(OtaClient::REBOOT_SETTLE_MS);
        REQUIRE(bench.client().phase() == OtaPhase::WAITING_BOARD);
    }
} // namespace

TEST_CASE("a bundle round trips through the reader")
{
    const ScratchDirectory directory;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 3U, "deadbeef", PROTOCOL_VERSION);
    const std::string path = directory.write("good.ota", built.bytes);

    OtaBundle bundle;
    std::string error;
    REQUIRE(loadOtaBundle(path, bundle, error));
    CHECK(error.empty());
    CHECK(bundle.loaded());
    CHECK(bundle.name == "drone_firmware");
    CHECK(bundle.mcuId == OTA_MCU_STM32F405);
    CHECK(bundle.gitHash == "deadbeef");
    CHECK(bundle.versionMajor == 3U);
    CHECK(bundle.protocolVersion == PROTOCOL_VERSION);
    REQUIRE(bundle.images.size() == OTA_SLOT_COUNT);
    CHECK(bundle.images[0].slot == OTA_SLOT_A);
    CHECK(bundle.images[1].slot == OTA_SLOT_B);
    CHECK(bundle.images[1].size == built.images[1].size());
    CHECK(bundle.images[1].bytes == built.images[1]);

    const OtaBundleImage *slotB = findOtaBundleImage(bundle, OTA_SLOT_B);
    REQUIRE(slotB != nullptr);
    CHECK(slotB->crc32 == otaImageCrc32(slotB->bytes.data(), slotB->bytes.size()));
    CHECK(findOtaBundleImage(bundle, OTA_SLOT_COUNT) == nullptr);
}

TEST_CASE("the reader refuses everything that is not a consistent bundle")
{
    const ScratchDirectory directory;
    OtaBundle bundle;
    std::string error;

    SECTION("a missing file")
    {
        CHECK(!(loadOtaBundle(directory.pathOf("absent.ota"), bundle, error)));
        CHECK(error.find("cannot read") != std::string::npos);
    }

    SECTION("a file that carries another magic")
    {
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", PROTOCOL_VERSION);
        built.bytes[0] = 'X';
        CHECK(!(loadOtaBundle(directory.write("bad.ota", built.bytes), bundle, error)));
        CHECK(error.find("not an .ota bundle") != std::string::npos);
    }

    SECTION("a bundle built against another wire version")
    {
        const BuiltBundle built = buildBundle(
            OTA_MCU_STM32F405, 1U, "abcd1234", static_cast<std::uint8_t>(PROTOCOL_VERSION - 1U));
        CHECK(!(loadOtaBundle(directory.write("old.ota", built.bytes), bundle, error)));
        CHECK(error.find("protocol version") != std::string::npos);
    }

    SECTION("an image whose bytes do not hash to the announced CRC")
    {
        const BuiltBundle built =
            buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", PROTOCOL_VERSION, OTA_SLOT_B);
        CHECK(!(loadOtaBundle(directory.write("crc.ota", built.bytes), bundle, error)));
        CHECK(error.find("crc32") != std::string::npos);
        // The refusal names the other plausible convention too, so an
        // integration mismatch is read off the message instead of guessed.
        CHECK(error.find("without the header") != std::string::npos);
    }

    SECTION("a bundle cut short of its last image")
    {
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", PROTOCOL_VERSION);
        built.bytes.resize(built.bytes.size() - 32U);
        CHECK(!(loadOtaBundle(directory.write("cut.ota", built.bytes), bundle, error)));
        CHECK(error.find("truncated") != std::string::npos);
    }

    SECTION("an image whose header was stamped for the other slot")
    {
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", PROTOCOL_VERSION);
        // Swap the two image bodies without touching the manifest: each
        // header then contradicts the entry it is filed under.
        const std::size_t firstAt =
            built.bytes.size() - built.images[0].size() - built.images[1].size() - 4U;
        std::memcpy(built.bytes.data() + firstAt, built.images[1].data(), OTA_IMAGE_HEADER_SIZE);
        CHECK(!(loadOtaBundle(directory.write("swap.ota", built.bytes), bundle, error)));
        CHECK(error.find("linked for slot") != std::string::npos);
    }
}

TEST_CASE("the happy path stages the image, trial boots it and confirms it")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);

    // The board was told the size and CRC of the slot B image, and only of
    // that one: the running slot is never written.
    const auto begin = bench.lastSent(PacketType::OTA_BEGIN);
    REQUIRE(begin.size() == OTA_BEGIN_PACKET_SIZE);
    OtaBeginPacket beginPacket{};
    std::memcpy(&beginPacket, begin.data(), sizeof(beginPacket));
    CHECK(beginPacket.imageSize == total);

    runTransfer(bench, total);
    // Every byte went out exactly once: no window was resent.
    const auto offsets = bench.chunkOffsets();
    CHECK(offsets.size() == (total + OTA_CHUNK_DATA_SIZE - 1U) / OTA_CHUNK_DATA_SIZE);
    for (std::size_t index = 0U; index < offsets.size(); ++index)
    {
        CHECK(offsets[index] == index * OTA_CHUNK_DATA_SIZE);
    }
    CHECK(bench.countSent(PacketType::OTA_FINISH) == 1U);

    runTrialBoot(bench);
    bench.feed(statusPacket(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
    REQUIRE(bench.client().phase() == OtaPhase::TESTING);
    CHECK(bench.countSent(PacketType::OTA_CONFIRM) == 0U);

    // Three more answers over three seconds is what the auto-confirm policy
    // asks for; nothing goes out before both are satisfied.
    for (int round = 0; round < 3; ++round)
    {
        bench.advance(OtaClient::STATUS_PERIOD_MS);
        if (bench.client().phase() == OtaPhase::TESTING &&
            bench.countSent(PacketType::OTA_CONFIRM) == 0U)
        {
            bench.feed(statusPacket(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
        }
    }
    bench.advance(OtaClient::STATUS_PERIOD_MS);
    REQUIRE(bench.countSent(PacketType::OTA_CONFIRM) == 1U);

    bench.feed(ackPacket(0U, PacketType::OTA_CONFIRM, OTA_RESULT_OK));
    CHECK(bench.client().phase() == OtaPhase::CONFIRMED);
    CHECK(bench.client().verdict() == OtaVerdict::CONFIRMED);
    CHECK(bench.client().verdictText().find("confirmed") != std::string::npos);
    CHECK(bench.client().lastError().empty());
}

TEST_CASE("a chunk acknowledgement that never comes sends the window again")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    const std::uint32_t session = bench.session();
    bench.feed(ackPacket(session, PacketType::OTA_BEGIN, OTA_RESULT_OK));
    REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);

    const std::uint32_t windowBytes =
        OTA_CHUNK_ACK_WINDOW * static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    REQUIRE(bench.client().progress().sentBytes == windowBytes);
    REQUIRE(bench.client().progress().ackedBytes == 0U);

    // The acknowledgement is lost. After the silence the sender goes back to
    // the last cumulative offset, which is still zero.
    bench.clearSent();
    bench.advance(OtaClient::CHUNK_ACK_TIMEOUT_MS);
    CHECK(bench.client().progress().retries == 1U);
    CHECK(bench.client().progress().sentBytes == windowBytes);
    const auto offsets = bench.chunkOffsets();
    REQUIRE(offsets.size() == OTA_CHUNK_ACK_WINDOW);
    CHECK(offsets.front() == 0U);

    // The resent window lands and the transfer carries on to the end.
    bench.feed(chunkAckPacket(session, windowBytes));
    CHECK(bench.client().progress().ackedBytes == windowBytes);
    while (bench.client().phase() == OtaPhase::TRANSFER)
    {
        bench.feed(chunkAckPacket(session, bench.client().progress().sentBytes));
    }
    CHECK(bench.client().phase() == OtaPhase::VERIFYING);
    CHECK(bench.client().progress().ackedBytes == total);
}

TEST_CASE("a repeated acknowledgement offset resends without waiting out the silence")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));
    const std::uint32_t session = bench.session();
    bench.feed(ackPacket(session, PacketType::OTA_BEGIN, OTA_RESULT_OK));

    const auto chunk = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    bench.feed(chunkAckPacket(session, 3U * chunk));
    REQUIRE(bench.client().progress().ackedBytes == 3U * chunk);
    bench.clearSent();

    // The board says it is still missing the same byte: an out-of-order
    // chunk was dropped, so everything above that offset goes again now.
    bench.feed(chunkAckPacket(session, 3U * chunk));
    CHECK(bench.client().progress().retries == 1U);
    const auto offsets = bench.chunkOffsets();
    REQUIRE_FALSE(offsets.empty());
    CHECK(offsets.front() == 3U * chunk);
}

TEST_CASE("one lost chunk echoes a whole window of repeats and costs one retry")
{
    // The board acknowledges every out-of-order chunk immediately, so one
    // radio loss makes every later chunk of the window repeat the same
    // offset. The bench proved that counting each echo as a refusal turns
    // a single loss into an instant failure: the echoes of an already
    // answered window must be ignored, and real progress must start the
    // stall budget over.
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));
    const std::uint32_t session = bench.session();
    bench.feed(ackPacket(session, PacketType::OTA_BEGIN, OTA_RESULT_OK));

    const auto chunk = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    bench.feed(chunkAckPacket(session, 3U * chunk));
    bench.clearSent();

    // A storm of repeats at one offset: one resend, one retry, not a fail.
    for (std::uint32_t echo = 0U; echo < 2U * OTA_CHUNK_ACK_WINDOW; ++echo)
    {
        bench.feed(chunkAckPacket(session, 3U * chunk));
    }
    CHECK(bench.client().progress().retries == 1U);
    CHECK(bench.client().phase() == OtaPhase::TRANSFER);

    // Progress re-arms the budget: storms after each of many losses never
    // add up to a failure as long as bytes keep landing in between.
    for (std::uint32_t round = 4U; round < 24U; ++round)
    {
        bench.feed(chunkAckPacket(session, round * chunk));
        for (std::uint32_t echo = 0U; echo < OTA_CHUNK_ACK_WINDOW; ++echo)
        {
            bench.feed(chunkAckPacket(session, round * chunk));
        }
        REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);
    }
}

TEST_CASE("an erase that never finishes fails the session and names the slot")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));
    REQUIRE(bench.client().phase() == OtaPhase::ERASING);

    bench.advance(OtaClient::BEGIN_TIMEOUT_MS - 1U);
    CHECK(bench.client().phase() == OtaPhase::ERASING);
    bench.advance(2U);
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().verdict() == OtaVerdict::FAILED);
    CHECK(bench.client().lastError().find("erasing slot B") != std::string::npos);
    CHECK(bench.client().verdictText().find("update failed") != std::string::npos);
}

TEST_CASE("a CRC refusal at the end of the transfer is reported in plain words")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);

    bench.feed(ackPacket(bench.session(), PacketType::OTA_FINISH, OTA_RESULT_CRC_MISMATCH));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError() == otaResultText(OTA_RESULT_CRC_MISMATCH));
    CHECK(bench.client().lastError().find("announced CRC") != std::string::npos);
    // Nothing was rebooted: an image that did not stage must not be booted.
    CHECK(bench.countSent(PacketType::REBOOT_COMMAND) == 0U);
}

TEST_CASE("a board that refuses because it is armed says exactly that")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));

    bench.feed(ackPacket(bench.session(), PacketType::OTA_BEGIN, OTA_RESULT_DENIED_ARMED));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("it is armed") != std::string::npos);
    CHECK(bench.countSent(PacketType::OTA_CHUNK) == 0U);
}

TEST_CASE("an answer to the end of the transfer that is a chunk ack resumes the transfer")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);
    REQUIRE(bench.client().phase() == OtaPhase::VERIFYING);

    // Bytes were still missing after all: the board says where from, and the
    // sender picks the transfer back up there.
    const std::uint32_t resumeAt = total - static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    const std::uint32_t session = bench.session();
    bench.clearSent();
    bench.feed(chunkAckPacket(session, resumeAt));
    CHECK(bench.client().phase() == OtaPhase::TRANSFER);
    CHECK(bench.client().progress().ackedBytes == resumeAt);
    const auto offsets = bench.chunkOffsets();
    REQUIRE_FALSE(offsets.empty());
    CHECK(offsets.front() == resumeAt);
}

TEST_CASE("a board that comes back on the old firmware is a rollback")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);
    runTrialBoot(bench);

    bench.feed(statusPacket(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
    CHECK(bench.client().phase() == OtaPhase::ROLLED_BACK);
    CHECK(bench.client().verdict() == OtaVerdict::ROLLED_BACK);
    CHECK(bench.client().verdictText().find("rolled back") != std::string::npos);
    // Nothing is confirmed on a rollback: the trial image is gone.
    CHECK(bench.countSent(PacketType::OTA_CONFIRM) == 0U);
}

TEST_CASE("a board that never comes back after the reboot fails the session")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);
    runTrialBoot(bench);

    bench.advance(OtaClient::BOARD_RETURN_TIMEOUT_MS);
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("did not come back") != std::string::npos);
}

TEST_CASE("manual confirm hands the last gesture to the operator")
{
    const ScratchDirectory directory;
    Bench bench;
    bench.client().setAutoConfirm(false);
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);
    runTrialBoot(bench);
    bench.feed(statusPacket(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
    REQUIRE(bench.client().phase() == OtaPhase::TESTING);

    for (int round = 0; round < 5; ++round)
    {
        bench.advance(OtaClient::STATUS_PERIOD_MS);
        bench.feed(statusPacket(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
    }
    // The link is long proven and nothing was sent: that is the whole point
    // of the manual mode.
    CHECK(bench.countSent(PacketType::OTA_CONFIRM) == 0U);
    CHECK(bench.client().confirmReady());

    std::string error;
    REQUIRE(bench.client().confirm(bench.nowUs(), error));
    CHECK(bench.countSent(PacketType::OTA_CONFIRM) == 1U);
    bench.feed(ackPacket(0U, PacketType::OTA_CONFIRM, OTA_RESULT_OK));
    CHECK(bench.client().phase() == OtaPhase::CONFIRMED);
}

TEST_CASE("a confirmation on a board that runs no trial image is refused")
{
    Bench bench;
    std::string error;
    CHECK(!(bench.client().confirm(bench.nowUs(), error)));
    CHECK(error.find("not running a trial image") != std::string::npos);
    CHECK(!(bench.client().abortSession(bench.nowUs(), error)));
    CHECK(error == "no update to abort");
}

TEST_CASE("an abort tells the board and frees the session")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));
    bench.feed(ackPacket(bench.session(), PacketType::OTA_BEGIN, OTA_RESULT_OK));
    REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);

    std::string error;
    REQUIRE(bench.client().abortSession(bench.nowUs(), error));
    CHECK(bench.countSent(PacketType::OTA_ABORT) == 1U);
    CHECK(bench.client().phase() == OtaPhase::IDLE);
    CHECK(bench.client().progress().totalBytes == 0U);
    CHECK(bench.client().lastError() == "aborted by the operator");
}

TEST_CASE("a revert flips the slot and reboots the board")
{
    Bench bench;
    std::string error;
    REQUIRE(bench.client().revert(bench.nowUs(), error));
    REQUIRE(bench.client().phase() == OtaPhase::REVERTING);
    CHECK(bench.countSent(PacketType::OTA_REVERT) == 1U);

    bench.feed(ackPacket(0U, PacketType::OTA_REVERT, OTA_RESULT_OK));
    CHECK(bench.countSent(PacketType::REBOOT_COMMAND) == 1U);
    CHECK(bench.client().verdict() == OtaVerdict::REVERTED);
    CHECK(bench.client().phase() == OtaPhase::IDLE);
}

TEST_CASE("a board that is not reachable fails the session instead of hanging")
{
    const ScratchDirectory directory;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, PROTOCOL_VERSION);
    Bench bench;
    const std::string path = directory.write("drone_firmware.ota", built.bytes);
    bench.setReachable(false);
    std::string error;
    CHECK(!(bench.client().start(path, bench.nowUs(), error)));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(error.find("serial link") != std::string::npos);
}

TEST_CASE("a bundle for another chip is refused before a single byte is sent")
{
    const ScratchDirectory directory;
    Bench bench;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F722, 2U, NEW_HASH, PROTOCOL_VERSION);
    const std::string path = directory.write("wrong_chip.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    bench.feed(statusPacket(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("mcu") != std::string::npos);
    CHECK(bench.countSent(PacketType::OTA_BEGIN) == 0U);
}

TEST_CASE("a board that already has a session open is not overwritten")
{
    const ScratchDirectory directory;
    Bench bench;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, PROTOCOL_VERSION);
    const std::string path = directory.write("drone_firmware.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    bench.feed(statusPacket(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH, true));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("already has an update session") != std::string::npos);
}

TEST_CASE("a board that never answers the initial query gives up saying so")
{
    const ScratchDirectory directory;
    Bench bench;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, PROTOCOL_VERSION);
    const std::string path = directory.write("drone_firmware.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    for (std::uint32_t round = 0U; round <= OtaClient::STATUS_TRIES; ++round)
    {
        bench.advance(OtaClient::STATUS_PERIOD_MS);
    }
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("did not answer a status request") != std::string::npos);
    CHECK(bench.countSent(PacketType::OTA_STATUS_REQUEST) == OtaClient::STATUS_TRIES);
}

TEST_CASE("the ota message carries the phase, the progress and the two identities")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    bench.feed(ackPacket(bench.session(), PacketType::OTA_BEGIN, OTA_RESULT_OK));
    bench.feed(chunkAckPacket(bench.session(), bench.client().progress().sentBytes));

    const std::string text = otaToJson(bench.client());
    CHECK(text.find(R"("type":"ota")") != std::string::npos);
    CHECK(text.find(R"("phase":"transfer")") != std::string::npos);
    CHECK(text.find(R"("verdict":"none")") != std::string::npos);
    CHECK(text.find(R"("targetSlot":1)") != std::string::npos);
    CHECK(text.find(R"("totalBytes":)" + std::to_string(total)) != std::string::npos);
    CHECK(text.find(R"("gitHash":")" + std::string(NEW_HASH) + R"(")") != std::string::npos);
    CHECK(text.find(R"("gitHash":")" + std::string(OLD_HASH) + R"(")") != std::string::npos);
    CHECK(text.find(R"("autoConfirm":true)") != std::string::npos);
    CHECK(text.find(R"("slotStateNames":["valid","empty"])") != std::string::npos);
}

TEST_CASE("the update messages decode into the requests the hub carries out")
{
    const auto asMessage = [](std::string_view text) {
        const auto decoded = parseClientMessage(text);
        REQUIRE(std::holds_alternative<ClientMessage>(decoded));
        return std::get<ClientMessage>(decoded);
    };

    const ClientMessage start =
        asMessage(R"({"type":"otaStart","id":1,"bundle":"/tmp/drone_firmware.ota"})");
    CHECK(start.type == ClientMessageType::OTA_START);
    CHECK(start.otaBundlePath == "/tmp/drone_firmware.ota");
    CHECK(start.target == StreamSource::FIRMWARE);

    const ClientMessage bare = asMessage(R"({"type":"otaStart"})");
    CHECK(bare.otaBundlePath.empty());

    const ClientMessage config = asMessage(R"({"type":"otaConfig","autoConfirm":false})");
    CHECK(config.type == ClientMessageType::OTA_CONFIG);
    CHECK(!(config.otaAutoConfirm));

    for (const auto &[text, type] : std::vector<std::pair<std::string, ClientMessageType>>{
             {R"({"type":"otaStatus"})", ClientMessageType::OTA_STATUS},
             {R"({"type":"otaAbort"})", ClientMessageType::OTA_ABORT},
             {R"({"type":"otaConfirm"})", ClientMessageType::OTA_CONFIRM},
             {R"({"type":"otaRevert"})", ClientMessageType::OTA_REVERT}})
    {
        CHECK(asMessage(text).type == type);
    }

    const auto refused = parseClientMessage(R"({"type":"otaConfig"})");
    REQUIRE(std::holds_alternative<std::string>(refused));
    CHECK(std::get<std::string>(refused).find("autoConfirm") != std::string::npos);

    const auto empty = parseClientMessage(R"({"type":"otaStart","bundle":""})");
    REQUIRE(std::holds_alternative<std::string>(empty));
    CHECK(std::get<std::string>(empty).find("bundle") != std::string::npos);
}
