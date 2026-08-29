/// @file
/// @brief The ground side of the firmware update: what the bundle reader
///        accepts and refuses, and what the session state machine does when
///        driven by a scripted board - the happy path, a lost chunk
///        acknowledgement, an erase that never finishes, a CRC refusal, a
///        board that refuses while armed, and the rollback verdict.
///
/// The client owns no socket and no clock: messages go into onEnvelope() and
/// time into tick(), so the fake board below is the whole test rig.
///
/// CHECK(!x) rather than CHECK_FALSE: that flag combination trips
/// clang-analyzer's enum-cast check inside Catch2's own headers.

#include <array>
#include <cstdint>
#include <cstdio>
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
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"

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
    /// @param buildEpoch build identity to stamp
    /// @param gitHash short hash to stamp
    /// @return the image bytes
    std::vector<std::uint8_t> makeImage(std::uint8_t slot,
                                        std::uint8_t mcuId,
                                        std::uint32_t buildEpoch,
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
        header.buildEpoch = buildEpoch;
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

    /// @return the 8 hex characters of this build's wire hash, as the
    ///         packaging script writes them
    std::string currentWireHash()
    {
        std::array<char, 9U> text{};
        static_cast<void>(std::snprintf(text.data(), text.size(), "%08x", WIRE_HASH));
        return text.data();
    }

    /// @brief Assembles a bundle file the way scripts/make_ota.py does.
    /// @param mcuId chip the build targets
    /// @param buildEpoch build identity of the build
    /// @param gitHash short hash of the build
    /// @param wireHash schema hash to announce, 8 hex characters
    /// @param breakCrcOfSlot slot whose announced CRC is deliberately wrong,
    ///        or 2 to keep both honest
    /// @return the file and its facts
    BuiltBundle buildBundle(std::uint8_t mcuId,
                            std::uint32_t buildEpoch,
                            const std::string &gitHash,
                            const std::string &wireHash,
                            std::uint8_t breakCrcOfSlot = 2U)
    {
        BuiltBundle built;
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            built.images[slot] = makeImage(slot, mcuId, buildEpoch, gitHash);
            built.crc[slot] = otaImageCrc32(built.images[slot].data(), built.images[slot].size());
            if (slot == breakCrcOfSlot)
            {
                built.crc[slot] ^= 1U;
            }
        }
        std::string manifest = R"({"name":"drone_firmware","mcuId":)";
        manifest += std::to_string(mcuId);
        manifest += R"(,"buildEpoch":)" + std::to_string(buildEpoch);
        manifest +=
            R"(,"gitHash":")" + gitHash + R"(","wireHash":")" + wireHash + R"(","images":[)";
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

    /// @brief Builds an envelope holding one empty body.
    /// @param tag mark4_Envelope_*_tag of the body
    /// @return the envelope
    mark4_Envelope bareEnvelope(pb_size_t tag)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = tag;
        return envelope;
    }

    /// @brief Builds one OtaStatus the way a board would send it.
    /// @param runningSlot slot the reported firmware runs from
    /// @param runningState state of that slot, in the flash encoding
    /// @param buildEpoch build identity of the running image
    /// @param gitHash short hash
    /// @param busy true to report an open transfer session
    /// @return the envelope
    mark4_Envelope statusEnvelope(std::uint8_t runningSlot,
                                  std::uint8_t runningState,
                                  std::uint32_t buildEpoch,
                                  const std::string &gitHash,
                                  bool busy = false)
    {
        mark4_Envelope envelope = bareEnvelope(mark4_Envelope_ota_status_tag);
        mark4_OtaStatus &status = envelope.body.ota_status;
        status.mcu = mark4_Mcu_STM32F405;
        status.running_slot = runningSlot;
        // During a trial boot the metadata still prefers the other slot;
        // everywhere else active and running coincide.
        status.active_slot = (runningState == OTA_SLOT_TESTING) ? 1U - runningSlot : runningSlot;
        status.updater_busy = busy;
        status.slots[runningSlot].state = otaSlotStateToWire(runningState);
        status.slots[1U - runningSlot].state = mark4_OtaSlotState_EMPTY;
        status.slots[runningSlot].build_epoch = buildEpoch;
        static_cast<void>(std::snprintf(status.slots[runningSlot].git_hash,
                                        sizeof(status.slots[runningSlot].git_hash),
                                        "%s",
                                        gitHash.c_str()));
        status.slot_size = TEST_SLOT_SIZE;
        status.max_chunk_data = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
        return envelope;
    }

    /// @brief Builds one OtaAck.
    /// @param session session nonce to echo
    /// @param op request being answered
    /// @param result outcome
    /// @return the envelope
    mark4_Envelope ackEnvelope(std::uint32_t session, mark4_OtaOp op, mark4_OtaResult result)
    {
        mark4_Envelope envelope = bareEnvelope(mark4_Envelope_ota_ack_tag);
        envelope.body.ota_ack.session = session;
        envelope.body.ota_ack.op = op;
        envelope.body.ota_ack.result = result;
        return envelope;
    }

    /// @brief Builds one OtaChunkAck.
    /// @param session session nonce to echo
    /// @param nextOffset first image byte still missing
    /// @return the envelope
    mark4_Envelope chunkAckEnvelope(std::uint32_t session, std::uint32_t nextOffset)
    {
        mark4_Envelope envelope = bareEnvelope(mark4_Envelope_ota_chunk_ack_tag);
        envelope.body.ota_chunk_ack.session = session;
        envelope.body.ota_chunk_ack.next_offset = nextOffset;
        return envelope;
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
            m_client.setSink([this](const mark4_Envelope &envelope, std::string &errorOut) {
                if (!m_reachable)
                {
                    errorOut = "no serial link to the board";
                    return false;
                }
                m_sent.push_back(envelope);
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

        /// @brief Hands one message to the client at the current instant.
        /// @param envelope message
        void feed(const mark4_Envelope &envelope)
        {
            REQUIRE(m_client.onEnvelope(envelope, m_nowUs));
        }

        /// @brief Cuts or restores the route to the board.
        /// @param on true when the board is reachable
        void setReachable(bool on)
        {
            m_reachable = on;
        }

        /// @param tag body tag to count
        /// @return how many messages with that body went out
        [[nodiscard]] std::size_t countSent(pb_size_t tag) const
        {
            std::size_t count = 0U;
            for (const auto &envelope : m_sent)
            {
                if (envelope.which_body == tag)
                {
                    ++count;
                }
            }
            return count;
        }

        /// @param tag body tag to look for
        /// @return the last message with that body, which_body 0 when none
        [[nodiscard]] mark4_Envelope lastSent(pb_size_t tag) const
        {
            mark4_Envelope found = mark4_Envelope_init_zero;
            for (const auto &entry : m_sent)
            {
                if (entry.which_body == tag)
                {
                    found = entry;
                }
            }
            return found;
        }

        /// @return the session nonce of the OtaBegin that went out
        [[nodiscard]] std::uint32_t session() const
        {
            const mark4_Envelope begin = lastSent(mark4_Envelope_ota_begin_tag);
            REQUIRE(begin.which_body == mark4_Envelope_ota_begin_tag);
            return begin.body.ota_begin.session;
        }

        /// @return the offsets of every OtaChunk that went out, in order
        [[nodiscard]] std::vector<std::uint32_t> chunkOffsets() const
        {
            std::vector<std::uint32_t> offsets;
            for (const auto &entry : m_sent)
            {
                if (entry.which_body == mark4_Envelope_ota_chunk_tag)
                {
                    offsets.push_back(entry.body.ota_chunk.offset);
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
        std::vector<mark4_Envelope> m_sent;
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
        const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, currentWireHash());
        const std::string path = directory.write("drone_firmware.ota", built.bytes);
        std::string error;
        REQUIRE(bench.client().start(path, bench.nowUs(), error));
        REQUIRE(bench.client().phase() == OtaPhase::QUERY);
        bench.feed(statusEnvelope(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
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
        bench.feed(ackEnvelope(session, mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));
        REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);
        while (bench.client().phase() == OtaPhase::TRANSFER)
        {
            const std::uint32_t sent = bench.client().progress().sentBytes;
            REQUIRE(sent > bench.client().progress().ackedBytes);
            bench.feed(chunkAckEnvelope(session, sent));
        }
        REQUIRE(bench.client().phase() == OtaPhase::VERIFYING);
        REQUIRE(bench.client().progress().ackedBytes == totalBytes);
    }

    /// @brief Takes the board through the trial boot up to TESTING.
    /// @param bench bench to drive
    void runTrialBoot(Bench &bench)
    {
        const std::uint32_t session = bench.session();
        bench.feed(ackEnvelope(session, mark4_OtaOp_FINISH, mark4_OtaResult_OTA_OK));
        REQUIRE(bench.client().phase() == OtaPhase::REBOOTING);
        REQUIRE(bench.countSent(mark4_Envelope_reboot_tag) == 1U);
        bench.advance(OtaClient::REBOOT_SETTLE_MS);
        REQUIRE(bench.client().phase() == OtaPhase::WAITING_BOARD);
    }
} // namespace

TEST_CASE("a bundle round trips through the reader")
{
    const ScratchDirectory directory;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 3U, "deadbeef", currentWireHash());
    const std::string path = directory.write("good.ota", built.bytes);

    OtaBundle bundle;
    std::string error;
    REQUIRE(loadOtaBundle(path, bundle, error));
    CHECK(error.empty());
    CHECK(bundle.loaded());
    CHECK(bundle.name == "drone_firmware");
    CHECK(bundle.mcuId == OTA_MCU_STM32F405);
    CHECK(bundle.gitHash == "deadbeef");
    CHECK(bundle.buildEpoch == 3U);
    CHECK(bundle.wireHash == currentWireHash());
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

TEST_CASE("a rebuilt bundle is picked up with no gesture, and a deleted one is forgotten")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::string path = directory.write(
        "watched.ota", buildBundle(OTA_MCU_STM32F405, 100U, "aaaaaaaa", currentWireHash()).bytes);
    bench.client().setDefaultBundlePath(path);

    // The first idle tick loads the bundle without any start.
    bench.advance(OtaClient::BUNDLE_CHECK_MS);
    REQUIRE(bench.client().bundle().loaded());
    CHECK(bench.client().bundle().buildEpoch == 100U);

    // A rebuild rewrites the file; the next check sees the new identity.
    static_cast<void>(directory.write(
        "watched.ota", buildBundle(OTA_MCU_STM32F405, 200U, "bbbbbbbb", currentWireHash()).bytes));
    bench.advance(OtaClient::BUNDLE_CHECK_MS);
    CHECK(bench.client().bundle().buildEpoch == 200U);
    CHECK(bench.client().bundle().gitHash == "bbbbbbbb");

    // The artifact disappears (a clean build tree): so does the display.
    std::filesystem::remove(path);
    bench.advance(OtaClient::BUNDLE_CHECK_MS);
    CHECK(!bench.client().bundle().loaded());
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
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", currentWireHash());
        built.bytes[0] = 'X';
        CHECK(!(loadOtaBundle(directory.write("bad.ota", built.bytes), bundle, error)));
        CHECK(error.find("not an .ota bundle") != std::string::npos);
    }

    SECTION("a bundle built against another wire schema")
    {
        const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", "00000000");
        CHECK(!(loadOtaBundle(directory.write("old.ota", built.bytes), bundle, error)));
        CHECK(error.find("wire") != std::string::npos);
    }

    SECTION("an image whose bytes do not hash to the announced CRC")
    {
        const BuiltBundle built =
            buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", currentWireHash(), OTA_SLOT_B);
        CHECK(!(loadOtaBundle(directory.write("crc.ota", built.bytes), bundle, error)));
        CHECK(error.find("crc32") != std::string::npos);
        // The refusal names the other plausible convention too, so an
        // integration mismatch is read off the message instead of guessed.
        CHECK(error.find("without the header") != std::string::npos);
    }

    SECTION("a bundle cut short of its last image")
    {
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", currentWireHash());
        built.bytes.resize(built.bytes.size() - 32U);
        CHECK(!(loadOtaBundle(directory.write("cut.ota", built.bytes), bundle, error)));
        CHECK(error.find("truncated") != std::string::npos);
    }

    SECTION("an image whose header was stamped for the other slot")
    {
        BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 1U, "abcd1234", currentWireHash());
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
    const mark4_Envelope begin = bench.lastSent(mark4_Envelope_ota_begin_tag);
    REQUIRE(begin.which_body == mark4_Envelope_ota_begin_tag);
    CHECK(begin.body.ota_begin.image_size == total);

    runTransfer(bench, total);
    // Every byte went out exactly once: no window was resent.
    const auto offsets = bench.chunkOffsets();
    CHECK(offsets.size() == (total + OTA_CHUNK_DATA_SIZE - 1U) / OTA_CHUNK_DATA_SIZE);
    for (std::size_t index = 0U; index < offsets.size(); ++index)
    {
        CHECK(offsets[index] == index * OTA_CHUNK_DATA_SIZE);
    }
    CHECK(bench.countSent(mark4_Envelope_ota_finish_tag) == 1U);

    runTrialBoot(bench);
    // The trial image confirms itself on the first request it serves, so
    // the very first status the hub sees already reports VALID; the hub
    // never sends a confirmation of its own.
    bench.feed(statusEnvelope(OTA_SLOT_B, OTA_SLOT_VALID, 2U, NEW_HASH));
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
    bench.feed(ackEnvelope(session, mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));
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
    bench.feed(chunkAckEnvelope(session, windowBytes));
    CHECK(bench.client().progress().ackedBytes == windowBytes);
    while (bench.client().phase() == OtaPhase::TRANSFER)
    {
        bench.feed(chunkAckEnvelope(session, bench.client().progress().sentBytes));
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
    bench.feed(ackEnvelope(session, mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));

    const auto chunk = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    bench.feed(chunkAckEnvelope(session, 3U * chunk));
    REQUIRE(bench.client().progress().ackedBytes == 3U * chunk);
    bench.clearSent();

    // The board says it is still missing the same byte: an out-of-order
    // chunk was dropped, so everything above that offset goes again now.
    bench.feed(chunkAckEnvelope(session, 3U * chunk));
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
    bench.feed(ackEnvelope(session, mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));

    const auto chunk = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);
    bench.feed(chunkAckEnvelope(session, 3U * chunk));
    bench.clearSent();

    // A storm of repeats at one offset: one resend, one retry, not a fail.
    for (std::uint32_t echo = 0U; echo < 2U * OTA_CHUNK_ACK_WINDOW; ++echo)
    {
        bench.feed(chunkAckEnvelope(session, 3U * chunk));
    }
    CHECK(bench.client().progress().retries == 1U);
    CHECK(bench.client().phase() == OtaPhase::TRANSFER);

    // Progress re-arms the budget: storms after each of many losses never
    // add up to a failure as long as bytes keep landing in between.
    for (std::uint32_t round = 4U; round < 24U; ++round)
    {
        bench.feed(chunkAckEnvelope(session, round * chunk));
        for (std::uint32_t echo = 0U; echo < OTA_CHUNK_ACK_WINDOW; ++echo)
        {
            bench.feed(chunkAckEnvelope(session, round * chunk));
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

    bench.feed(ackEnvelope(bench.session(), mark4_OtaOp_FINISH, mark4_OtaResult_CRC_MISMATCH));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError() == otaResultText(mark4_OtaResult_CRC_MISMATCH));
    CHECK(bench.client().lastError().find("announced CRC") != std::string::npos);
    // Nothing was rebooted: an image that did not stage must not be booted.
    CHECK(bench.countSent(mark4_Envelope_reboot_tag) == 0U);
}

TEST_CASE("a board that refuses because it is armed says exactly that")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));

    bench.feed(ackEnvelope(bench.session(), mark4_OtaOp_BEGIN, mark4_OtaResult_DENIED_ARMED));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("it is armed") != std::string::npos);
    CHECK(bench.countSent(mark4_Envelope_ota_chunk_tag) == 0U);
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
    bench.feed(chunkAckEnvelope(session, resumeAt));
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

    bench.feed(statusEnvelope(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
    CHECK(bench.client().phase() == OtaPhase::ROLLED_BACK);
    CHECK(bench.client().verdict() == OtaVerdict::ROLLED_BACK);
    CHECK(bench.client().verdictText().find("rolled back") != std::string::npos);
    // Nothing is confirmed on a rollback: the trial image is gone.
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

TEST_CASE("a trial that has not vouched for itself yet keeps the hub polling")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    runTransfer(bench, total);
    runTrialBoot(bench);
    // A board that answers but still says TESTING has not reached its own
    // checkpoint (or its metadata write raced this answer): the hub stays
    // in TESTING and keeps asking, sending nothing.
    bench.feed(statusEnvelope(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
    REQUIRE(bench.client().phase() == OtaPhase::TESTING);

    for (int round = 0; round < 3; ++round)
    {
        bench.advance(OtaClient::STATUS_PERIOD_MS);
        bench.feed(statusEnvelope(OTA_SLOT_B, OTA_SLOT_TESTING, 2U, NEW_HASH));
        REQUIRE(bench.client().phase() == OtaPhase::TESTING);
    }

    // The image finally vouches for itself; the next answer says so.
    bench.advance(OtaClient::STATUS_PERIOD_MS);
    bench.feed(statusEnvelope(OTA_SLOT_B, OTA_SLOT_VALID, 2U, NEW_HASH));
    CHECK(bench.client().phase() == OtaPhase::CONFIRMED);
    CHECK(bench.client().verdict() == OtaVerdict::CONFIRMED);
}

TEST_CASE("an abort with no update running is refused")
{
    Bench bench;
    std::string error;
    CHECK(!(bench.client().abortSession(bench.nowUs(), error)));
    CHECK(error == "no update to abort");
}

TEST_CASE("an abort tells the board and frees the session")
{
    const ScratchDirectory directory;
    Bench bench;
    static_cast<void>(startHappySession(bench, directory));
    bench.feed(ackEnvelope(bench.session(), mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));
    REQUIRE(bench.client().phase() == OtaPhase::TRANSFER);

    std::string error;
    REQUIRE(bench.client().abortSession(bench.nowUs(), error));
    CHECK(bench.countSent(mark4_Envelope_ota_abort_tag) == 1U);
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
    CHECK(bench.countSent(mark4_Envelope_ota_revert_tag) == 1U);

    bench.feed(ackEnvelope(0U, mark4_OtaOp_REVERT, mark4_OtaResult_OTA_OK));
    CHECK(bench.countSent(mark4_Envelope_reboot_tag) == 1U);
    CHECK(bench.client().verdict() == OtaVerdict::REVERTED);
    CHECK(bench.client().phase() == OtaPhase::IDLE);
}

TEST_CASE("a board that is not reachable fails the session instead of hanging")
{
    const ScratchDirectory directory;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, currentWireHash());
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
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F722, 2U, NEW_HASH, currentWireHash());
    const std::string path = directory.write("wrong_chip.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    bench.feed(statusEnvelope(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("mcu") != std::string::npos);
    CHECK(bench.countSent(mark4_Envelope_ota_begin_tag) == 0U);
}

TEST_CASE("a board that already has a session open is not overwritten")
{
    const ScratchDirectory directory;
    Bench bench;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, currentWireHash());
    const std::string path = directory.write("drone_firmware.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    bench.feed(statusEnvelope(OTA_SLOT_A, OTA_SLOT_VALID, 1U, OLD_HASH, true));
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("already has an update session") != std::string::npos);
}

TEST_CASE("a board that never answers the initial query gives up saying so")
{
    const ScratchDirectory directory;
    Bench bench;
    const BuiltBundle built = buildBundle(OTA_MCU_STM32F405, 2U, NEW_HASH, currentWireHash());
    const std::string path = directory.write("drone_firmware.ota", built.bytes);
    std::string error;
    REQUIRE(bench.client().start(path, bench.nowUs(), error));
    for (std::uint32_t round = 0U; round <= OtaClient::STATUS_TRIES; ++round)
    {
        bench.advance(OtaClient::STATUS_PERIOD_MS);
    }
    CHECK(bench.client().phase() == OtaPhase::FAILED);
    CHECK(bench.client().lastError().find("did not answer a status request") != std::string::npos);
    CHECK(bench.countSent(mark4_Envelope_ota_status_request_tag) == OtaClient::STATUS_TRIES);
}

TEST_CASE("the ota message carries the phase, the progress and the two identities")
{
    const ScratchDirectory directory;
    Bench bench;
    const std::uint32_t total = startHappySession(bench, directory);
    bench.feed(ackEnvelope(bench.session(), mark4_OtaOp_BEGIN, mark4_OtaResult_OTA_OK));
    bench.feed(chunkAckEnvelope(bench.session(), bench.client().progress().sentBytes));

    const std::string text = otaToJson(bench.client());
    CHECK(text.find(R"("type":"ota")") != std::string::npos);
    CHECK(text.find(R"("phase":"transfer")") != std::string::npos);
    CHECK(text.find(R"("verdict":"none")") != std::string::npos);
    CHECK(text.find(R"("targetSlot":1)") != std::string::npos);
    CHECK(text.find(R"("totalBytes":)" + std::to_string(total)) != std::string::npos);
    CHECK(text.find(R"("gitHash":")" + std::string(NEW_HASH) + R"(")") != std::string::npos);
    CHECK(text.find(R"("gitHash":")" + std::string(OLD_HASH) + R"(")") != std::string::npos);
    CHECK(text.find(R"("stateName":"valid")") != std::string::npos);
    CHECK(text.find(R"("stateName":"empty")") != std::string::npos);
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
    CHECK(start.target == mark4_NodeKind_FIRMWARE);

    const ClientMessage bare = asMessage(R"({"type":"otaStart"})");
    CHECK(bare.otaBundlePath.empty());

    for (const auto &[text, type] : std::vector<std::pair<std::string, ClientMessageType>>{
             {R"({"type":"otaStatus"})", ClientMessageType::OTA_STATUS},
             {R"({"type":"otaAbort"})", ClientMessageType::OTA_ABORT},
             {R"({"type":"otaRevert"})", ClientMessageType::OTA_REVERT}})
    {
        CHECK(asMessage(text).type == type);
    }

    // The board confirms its own trial now: the old ground-side gestures
    // are gone from the message set.
    const auto refused = parseClientMessage(R"({"type":"otaConfirm"})");
    REQUIRE(std::holds_alternative<std::string>(refused));
    CHECK(std::get<std::string>(refused).find("unknown message type") != std::string::npos);

    const auto empty = parseClientMessage(R"({"type":"otaStart","bundle":""})");
    REQUIRE(std::holds_alternative<std::string>(empty));
    CHECK(std::get<std::string>(empty).find("bundle") != std::string::npos);
}
