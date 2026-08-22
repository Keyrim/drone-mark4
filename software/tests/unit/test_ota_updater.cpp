/// @file
/// @brief The board-side update session, packet in and packet out. This is
///        where the failure table of the OTA design stops being a promise: a
///        lost chunk, a duplicate, a corrupted transfer, a store that dies
///        mid-write, a link that goes silent, a trial boot that is never
///        confirmed - each one has a test here, against a RAM store that can
///        be made to fail on command and against the file-backed store the
///        desktop flight processes really use.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "platform/firmware_store.hpp"
#include "platform_common/crc32_mpeg2.hpp"
#include "platform_common/ota_updater.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"

namespace
{
    constexpr std::uint32_t TEST_SLOT_SIZE = 8192U;
    constexpr std::uint32_t TEST_SESSION = 0xA5A5F00DU;
    constexpr std::uint32_t OTHER_SESSION = 0x0BADF00DU;

    /// Data bytes of one full chunk.
    constexpr std::uint32_t CHUNK_BYTES = mark4::OTA_CHUNK_DATA_SIZE;

    /// An image big enough to cross one full acknowledgement window and land
    /// its last bytes in the middle of the next one.
    constexpr std::uint32_t TEST_IMAGE_SIZE = (17U * CHUNK_BYTES) + 100U;

    /// Sentinel meaning "no programming failure armed".
    constexpr std::uint32_t NO_FAULT = 0xFFFFFFFFU;

    /// Firmware store in RAM, with a fault switch on every operation the
    /// updater depends on. It enforces the same contract as the real ones -
    /// the running slot is untouchable, programming needs an erase and moves
    /// forward only - so a test failure here is the updater misbehaving, not
    /// the double being lenient.
    class FakeStore final : public mark4::AbsFirmwareStore
    {
      public:
        /// @param runningSlot slot the fake firmware executes from
        /// @param slotSize bytes per slot
        FakeStore(std::uint8_t runningSlot, std::uint32_t slotSize)
            : m_runningSlot(runningSlot),
              m_slotSize(slotSize)
        {
            for (auto &slot : m_slots)
            {
                slot.assign(slotSize, 0xFFU);
            }
        }

        [[nodiscard]] std::uint8_t runningSlot() const override
        {
            return m_runningSlot;
        }

        [[nodiscard]] std::uint32_t slotSize() const override
        {
            return m_slotSize;
        }

        [[nodiscard]] std::uint8_t mcuId() const override
        {
            return mark4::OTA_MCU_SIM;
        }

        bool eraseSlot(std::uint8_t slot) override
        {
            if (slot >= mark4::OTA_SLOT_COUNT || slot == m_runningSlot || m_failErase)
            {
                return false;
            }
            m_slots.at(slot).assign(m_slotSize, 0xFFU);
            m_erased.at(slot) = true;
            m_programEnd.at(slot) = 0U;
            ++m_eraseCount.at(slot);
            return true;
        }

        bool program(std::uint8_t slot,
                     std::uint32_t offset,
                     const std::uint8_t *data,
                     std::uint32_t size) override
        {
            if (slot >= mark4::OTA_SLOT_COUNT || slot == m_runningSlot || data == nullptr)
            {
                return false;
            }
            if (!m_erased.at(slot) || offset != m_programEnd.at(slot))
            {
                return false; // the updater programs strictly sequentially
            }
            if (offset > m_slotSize || (m_slotSize - offset) < size)
            {
                return false;
            }
            if (offset >= m_failProgramFrom)
            {
                return false;
            }
            std::memcpy(&m_slots.at(slot).at(offset), data, size);
            m_programEnd.at(slot) = offset + size;
            return true;
        }

        bool read(std::uint8_t slot,
                  std::uint32_t offset,
                  std::uint8_t *dataOut,
                  std::uint32_t size) const override
        {
            if (slot >= mark4::OTA_SLOT_COUNT || dataOut == nullptr || m_failRead)
            {
                return false;
            }
            if (offset > m_slotSize || (m_slotSize - offset) < size)
            {
                return false;
            }
            std::memcpy(dataOut, &m_slots.at(slot).at(offset), size);
            return true;
        }

        [[nodiscard]] std::uint32_t crc32(std::uint8_t slot,
                                          std::uint32_t offset,
                                          std::uint32_t size) const override
        {
            mark4::Crc32Mpeg2 crc;
            if (slot < mark4::OTA_SLOT_COUNT && offset <= m_slotSize &&
                (m_slotSize - offset) >= size)
            {
                crc.update(&m_slots.at(slot).at(offset), size);
            }
            return crc.finish();
        }

        bool readMeta(mark4::OtaMetaState &stateOut) const override
        {
            if (m_failMetaRead)
            {
                return false;
            }
            stateOut = m_meta;
            return true;
        }

        bool writeMeta(const mark4::OtaMetaState &state) override
        {
            if (m_metaWritesLeft == 0U)
            {
                return false;
            }
            if (m_metaWritesLeft != NO_FAULT)
            {
                --m_metaWritesLeft;
            }
            m_meta = state;
            ++m_metaWriteCount;
            return true;
        }

        /// @brief Writes slot bytes behind the store's own rules, the way a
        ///        linker or a bootloader would.
        /// @param slot slot to write
        /// @param offset byte offset from the slot base
        /// @param data bytes to place
        /// @param size byte count
        void poke(std::uint8_t slot,
                  std::uint32_t offset,
                  const std::uint8_t *data,
                  std::size_t size)
        {
            std::memcpy(&m_slots.at(slot).at(offset), data, size);
        }

        /// @brief Replaces the metadata, the way the bootloader does at boot.
        /// @param state new metadata content
        void forceMeta(const mark4::OtaMetaState &state)
        {
            m_meta = state;
        }

        /// @param slot slot index
        /// @param offset byte offset from the slot base
        /// @param size byte count
        /// @return a copy of those slot bytes
        [[nodiscard]] std::vector<std::uint8_t> slotBytes(std::uint8_t slot,
                                                          std::uint32_t offset,
                                                          std::uint32_t size) const
        {
            const std::vector<std::uint8_t> &content = m_slots.at(slot);
            return {content.begin() + offset, content.begin() + offset + size};
        }

        /// @return the metadata as it stands
        [[nodiscard]] const mark4::OtaMetaState &meta() const
        {
            return m_meta;
        }

        /// @param slot slot index
        /// @return how many times that slot was erased
        [[nodiscard]] std::uint32_t eraseCount(std::uint8_t slot) const
        {
            return m_eraseCount.at(slot);
        }

        /// @return how many metadata records were appended
        [[nodiscard]] std::uint32_t metaWriteCount() const
        {
            return m_metaWriteCount;
        }

        /// @brief Makes every erase fail from now on.
        void failErases()
        {
            m_failErase = true;
        }

        /// @brief Makes every slot read fail from now on.
        void failReads()
        {
            m_failRead = true;
        }

        /// @brief Makes every metadata read fail from now on.
        void failMetaReads()
        {
            m_failMetaRead = true;
        }

        /// @brief Lets a few metadata appends through, then fails them all.
        /// @param allowed appends still accepted
        void allowMetaWrites(std::uint32_t allowed)
        {
            m_metaWritesLeft = allowed;
        }

        /// @brief Makes programming fail from one offset on: the store dying
        ///        in the middle of a transfer.
        /// @param offset first offset that must fail
        void failProgramFrom(std::uint32_t offset)
        {
            m_failProgramFrom = offset;
        }

      private:
        std::uint8_t m_runningSlot;                                           ///< slot in use
        std::uint32_t m_slotSize;                                             ///< bytes per slot
        std::array<std::vector<std::uint8_t>, mark4::OTA_SLOT_COUNT> m_slots; ///< slot contents
        std::array<bool, mark4::OTA_SLOT_COUNT> m_erased{};              ///< erased since start
        std::array<std::uint32_t, mark4::OTA_SLOT_COUNT> m_programEnd{}; ///< next legal offset
        std::array<std::uint32_t, mark4::OTA_SLOT_COUNT> m_eraseCount{}; ///< erases per slot
        mark4::OtaMetaState m_meta{};               ///< the newest record, logically
        std::uint32_t m_metaWriteCount = 0U;        ///< appends accepted
        std::uint32_t m_metaWritesLeft = NO_FAULT;  ///< appends still accepted
        std::uint32_t m_failProgramFrom = NO_FAULT; ///< first failing program offset
        bool m_failErase = false;                   ///< erases answer false
        bool m_failRead = false;                    ///< slot reads answer false
        bool m_failMetaRead = false;                ///< metadata reads answer false
    };

    /// One answer coming back from the updater.
    struct Reply
    {
        bool consumed = false;             ///< the packet was an OTA packet
        std::vector<std::uint8_t> bytes{}; ///< the reply, empty when there is none
    };

    /// @brief Hands one packet to the updater and collects the answer.
    /// @param updater updater under test
    /// @param packet packet bytes
    /// @param size packet size
    /// @param in facts of the moment
    /// @return what came back
    Reply feed(mark4::OtaUpdater &updater,
               const std::uint8_t *packet,
               std::size_t size,
               const mark4::OtaUpdater::Inputs &in)
    {
        std::array<std::uint8_t, 256U> buffer{};
        Reply reply;
        const std::size_t written =
            updater.handle(packet, size, in, buffer.data(), buffer.size(), reply.consumed);
        reply.bytes.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
        return reply;
    }

    /// @brief Serializes a wire struct into a byte array.
    /// @tparam Packet wire struct type
    /// @param packet packet to serialize
    /// @return its bytes
    template <typename Packet>
    std::array<std::uint8_t, sizeof(Packet)> bytesOf(const Packet &packet)
    {
        std::array<std::uint8_t, sizeof(Packet)> bytes{};
        std::memcpy(bytes.data(), &packet, bytes.size());
        return bytes;
    }

    /// @brief Builds an OTA_BEGIN packet.
    /// @param session session nonce
    /// @param imageSize announced image size
    /// @param imageCrc announced image CRC
    /// @return the packet bytes
    std::array<std::uint8_t, mark4::OTA_BEGIN_PACKET_SIZE> beginPacket(std::uint32_t session,
                                                                       std::uint32_t imageSize,
                                                                       std::uint32_t imageCrc)
    {
        mark4::OtaBeginPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_BEGIN);
        packet.session = session;
        packet.imageSize = imageSize;
        packet.imageCrc = imageCrc;
        return bytesOf(packet);
    }

    /// @brief Builds an OTA_CHUNK packet.
    /// @param session session nonce
    /// @param offset image offset of the chunk
    /// @param data chunk bytes, may be nullptr when length is 0
    /// @param length valid bytes in data
    /// @return the packet bytes
    std::array<std::uint8_t, mark4::OTA_CHUNK_PACKET_SIZE> chunkPacket(std::uint32_t session,
                                                                       std::uint32_t offset,
                                                                       const std::uint8_t *data,
                                                                       std::uint32_t length)
    {
        mark4::OtaChunkPacket packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(mark4::PacketType::OTA_CHUNK);
        packet.session = session;
        packet.offset = offset;
        packet.length = static_cast<std::uint8_t>(length);
        packet.data.fill(0U);
        if (data != nullptr && length > 0U)
        {
            std::memcpy(packet.data.data(), data, length);
        }
        return bytesOf(packet);
    }

    /// @brief Builds a packet that carries nothing but a session nonce.
    /// @tparam Packet wire struct type
    /// @param type packet type
    /// @param session session nonce
    /// @return the packet bytes
    template <typename Packet>
    std::array<std::uint8_t, sizeof(Packet)> sessionPacket(mark4::PacketType type,
                                                           std::uint32_t session)
    {
        Packet packet{};
        packet.version = mark4::PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(type);
        packet.session = session;
        return bytesOf(packet);
    }

    /// @brief Builds one of the header-only packets.
    /// @param type packet type
    /// @return the two packet bytes
    std::array<std::uint8_t, 2U> barePacket(mark4::PacketType type)
    {
        return {mark4::PROTOCOL_VERSION, static_cast<std::uint8_t>(type)};
    }

    // The reply packets are byte-packed, so their multi-byte fields sit at
    // unaligned offsets and a test may never bind a reference to one (which
    // is what a Catch2 comparison does). Every decoder therefore hands back
    // an ordinary struct with the same field names, copied out field by
    // field.

    /// One decoded OtaAckPacket, in aligned fields.
    struct Ack
    {
        std::uint32_t session = 0U;  ///< session nonce echoed, 0 when none
        std::uint8_t ackedType = 0U; ///< request being answered
        std::uint8_t result = 0U;    ///< OTA_RESULT_*
    };

    /// One decoded OtaChunkAckPacket, in aligned fields.
    struct ChunkAck
    {
        std::uint32_t session = 0U;    ///< session nonce echoed
        std::uint32_t nextOffset = 0U; ///< first image byte still missing
    };

    /// One decoded OtaStatusPacket, in aligned fields.
    struct Status
    {
        std::uint8_t mcuId = 0U;                                     ///< board identity
        std::uint8_t runningSlot = 0U;                               ///< slot in use
        std::array<std::uint8_t, mark4::OTA_SLOT_COUNT> slotState{}; ///< OTA_SLOT_* per slot
        std::uint8_t updaterBusy = 0U;                               ///< a session is open
        std::uint8_t versionMajor = 0U;                              ///< running version
        std::uint8_t versionMinor = 0U;                              ///< running version
        std::uint8_t versionPatch = 0U;                              ///< running version
        std::array<char, mark4::OTA_GIT_HASH_SIZE> gitHash{};        ///< running git hash
        std::uint32_t slotSize = 0U;                                 ///< bytes per slot
        std::uint16_t maxChunkData = 0U;                             ///< largest chunk accepted
    };

    /// @brief Decodes an ack, checking it is one.
    /// @param reply reply to decode
    /// @return the ack
    Ack decodeAck(const Reply &reply)
    {
        REQUIRE(reply.consumed);
        REQUIRE(reply.bytes.size() == mark4::OTA_ACK_PACKET_SIZE);
        REQUIRE(reply.bytes[0] == mark4::PROTOCOL_VERSION);
        REQUIRE(reply.bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::OTA_ACK));
        mark4::OtaAckPacket packet{};
        std::memcpy(&packet, reply.bytes.data(), sizeof(packet));

        Ack decoded;
        decoded.session = packet.session;
        decoded.ackedType = packet.ackedType;
        decoded.result = packet.result;
        return decoded;
    }

    /// @brief Decodes a chunk acknowledgement, checking it is one.
    /// @param reply reply to decode
    /// @return the acknowledgement
    ChunkAck decodeChunkAck(const Reply &reply)
    {
        REQUIRE(reply.consumed);
        REQUIRE(reply.bytes.size() == mark4::OTA_CHUNK_ACK_PACKET_SIZE);
        REQUIRE(reply.bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::OTA_CHUNK_ACK));
        mark4::OtaChunkAckPacket packet{};
        std::memcpy(&packet, reply.bytes.data(), sizeof(packet));

        ChunkAck decoded;
        decoded.session = packet.session;
        decoded.nextOffset = packet.nextOffset;
        return decoded;
    }

    /// @brief Decodes a status packet, checking it is one.
    /// @param reply reply to decode
    /// @return the status
    Status decodeStatus(const Reply &reply)
    {
        REQUIRE(reply.consumed);
        REQUIRE(reply.bytes.size() == mark4::OTA_STATUS_PACKET_SIZE);
        REQUIRE(reply.bytes[1] == static_cast<std::uint8_t>(mark4::PacketType::OTA_STATUS));
        mark4::OtaStatusPacket packet{};
        std::memcpy(&packet, reply.bytes.data(), sizeof(packet));

        Status decoded;
        decoded.mcuId = packet.mcuId;
        decoded.runningSlot = packet.runningSlot;
        std::memcpy(decoded.slotState.data(), &packet.slotState, mark4::OTA_SLOT_COUNT);
        decoded.updaterBusy = packet.updaterBusy;
        decoded.versionMajor = packet.versionMajor;
        decoded.versionMinor = packet.versionMinor;
        decoded.versionPatch = packet.versionPatch;
        std::memcpy(decoded.gitHash.data(), &packet.gitHash, mark4::OTA_GIT_HASH_SIZE);
        decoded.slotSize = packet.slotSize;
        decoded.maxChunkData = packet.maxChunkData;
        return decoded;
    }

    /// What an image is allowed to disagree about, one field per failure the
    /// updater has to catch.
    struct ImageSpec
    {
        std::uint32_t totalSize = TEST_IMAGE_SIZE;    ///< bytes actually sent
        std::uint32_t declaredSize = 0U;              ///< header size field, 0 = totalSize
        std::uint8_t slotId = mark4::OTA_SLOT_B;      ///< slot the image was linked for
        std::uint8_t mcuId = mark4::OTA_MCU_SIM;      ///< chip the image was built for
        std::uint32_t magic = mark4::OTA_IMAGE_MAGIC; ///< header magic
        std::uint16_t headerVersion = mark4::OTA_IMAGE_HEADER_VERSION; ///< header layout
        bool stamped = true; ///< the packaging step filled the CRC fields
    };

    /// @brief Builds a complete image: a stamped header then filler bytes.
    /// @param spec what the image should claim
    /// @return the image bytes
    std::vector<std::uint8_t> buildImage(const ImageSpec &spec)
    {
        std::vector<std::uint8_t> image(spec.totalSize, 0xFFU);
        for (std::uint32_t i = mark4::OTA_IMAGE_HEADER_SIZE; i < spec.totalSize; ++i)
        {
            image[i] = static_cast<std::uint8_t>((i * 31U) & 0xFFU);
        }

        mark4::OtaImageHeader header{};
        header.magic = spec.magic;
        header.headerVersion = spec.headerVersion;
        header.mcuId = spec.mcuId;
        header.slotId = spec.slotId;
        header.imageSize = (spec.declaredSize == 0U) ? spec.totalSize : spec.declaredSize;
        header.versionMajor = 1U;
        header.versionMinor = 2U;
        header.versionPatch = 3U;
        header.reserved0 = 0xFFU;
        const std::array<char, mark4::OTA_GIT_HASH_SIZE> hash = {
            'd', 'e', 'a', 'd', 'b', 'e', 'e', 'f'};
        std::memcpy(&header.gitHash, hash.data(), hash.size());
        header.reserved.fill(0xFFU);
        header.imageCrc = mark4::OTA_IMAGE_UNSTAMPED;
        header.headerCrc = mark4::OTA_IMAGE_UNSTAMPED;
        std::memcpy(image.data(), &header, sizeof(header));

        if (spec.stamped)
        {
            // What scripts/make_ota.py does: the payload CRC first, then the
            // header CRC over everything before it.
            const std::uint32_t payloadCrc =
                mark4::crc32Mpeg2(image.data() + mark4::OTA_IMAGE_HEADER_SIZE,
                                  image.size() - mark4::OTA_IMAGE_HEADER_SIZE);
            std::memcpy(image.data() + offsetof(mark4::OtaImageHeader, imageCrc),
                        &payloadCrc,
                        sizeof(payloadCrc));
            const std::uint32_t headerCrc =
                mark4::crc32Mpeg2(image.data(), offsetof(mark4::OtaImageHeader, headerCrc));
            std::memcpy(image.data() + offsetof(mark4::OtaImageHeader, headerCrc),
                        &headerCrc,
                        sizeof(headerCrc));
        }
        return image;
    }

    /// @param image image bytes
    /// @return the whole-image CRC an OTA_BEGIN announces
    std::uint32_t imageCrc(const std::vector<std::uint8_t> &image)
    {
        return mark4::crc32Mpeg2(image.data(), image.size());
    }

    /// @brief Streams an image in order, one full chunk at a time.
    /// @param updater updater under test
    /// @param session session nonce
    /// @param image image bytes
    /// @param in facts of the moment
    /// @param upTo bytes to send at most
    /// @return the offsets carried by the acknowledgements that came back
    std::vector<std::uint32_t> streamImage(mark4::OtaUpdater &updater,
                                           std::uint32_t session,
                                           const std::vector<std::uint8_t> &image,
                                           const mark4::OtaUpdater::Inputs &in,
                                           std::uint32_t upTo)
    {
        std::vector<std::uint32_t> acks;
        std::uint32_t offset = 0U;
        while (offset < upTo)
        {
            const std::uint32_t length = std::min<std::uint32_t>(CHUNK_BYTES, upTo - offset);
            const auto packet = chunkPacket(session, offset, &image[offset], length);
            const Reply reply = feed(updater, packet.data(), packet.size(), in);
            REQUIRE(reply.consumed);
            if (!reply.bytes.empty())
            {
                acks.push_back(decodeChunkAck(reply).nextOffset);
            }
            offset += length;
        }
        return acks;
    }

    /// @brief Private directory for one test, emptied beforehand.
    /// @param name directory name
    /// @return the path
    std::string scratchDirectory(const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::error_code failure;
        std::filesystem::remove_all(path, failure);
        return path.string();
    }
} // namespace

TEST_CASE("the whole transfer, from begin to a staged slot")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    const Ack beginAck = decodeAck(feed(updater, begin.data(), begin.size(), in));
    REQUIRE(beginAck.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_BEGIN));
    REQUIRE(beginAck.result == mark4::OTA_RESULT_OK);
    REQUIRE(beginAck.session == TEST_SESSION);
    REQUIRE(updater.sessionActive());

    // The target slot was erased, and the metadata says so before the slot
    // content stopped being trustworthy: no fallback image is claimed to
    // exist where one was just wiped.
    REQUIRE(store.eraseCount(mark4::OTA_SLOT_B) == 1U);
    REQUIRE(store.eraseCount(mark4::OTA_SLOT_A) == 0U);
    REQUIRE(store.metaWriteCount() == 1U);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(store.meta().activeSlot == mark4::OTA_SLOT_A);

    const std::vector<std::uint32_t> acks =
        streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE);

    // One acknowledgement per window, plus one the moment the last image byte
    // lands: that last one is what tells the sender to finish.
    const std::uint32_t windowBytes = mark4::OTA_CHUNK_ACK_WINDOW * mark4::OTA_CHUNK_DATA_SIZE;
    REQUIRE(acks.size() == 2U);
    REQUIRE(acks[0] == windowBytes);
    REQUIRE(acks[1] == TEST_IMAGE_SIZE);

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    const Ack finishAck = decodeAck(feed(updater, finish.data(), finish.size(), in));
    REQUIRE(finishAck.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_FINISH));
    REQUIRE(finishAck.result == mark4::OTA_RESULT_OK);
    REQUIRE(finishAck.session == TEST_SESSION);
    REQUIRE(!updater.sessionActive());

    REQUIRE(store.slotBytes(mark4::OTA_SLOT_B, 0U, TEST_IMAGE_SIZE) == image);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(store.meta().activeSlot == mark4::OTA_SLOT_A); // the trial boot decides that
    REQUIRE(!store.meta().trialAttempted);
    REQUIRE(store.metaWriteCount() == 2U);
}

TEST_CASE("a duplicate chunk changes nothing and repeats what is expected")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);

    const std::uint32_t length = CHUNK_BYTES;
    const auto first = chunkPacket(TEST_SESSION, 0U, image.data(), length);
    REQUIRE(feed(updater, first.data(), first.size(), in).bytes.empty()); // inside the window
    REQUIRE(decodeChunkAck(feed(updater, first.data(), first.size(), in)).nextOffset == length);

    // The slot only ever took the chunk once: a second program at the same
    // offset would have been refused by the store and killed the session.
    REQUIRE(updater.sessionActive());
    REQUIRE(store.slotBytes(mark4::OTA_SLOT_B, 0U, length) ==
            std::vector<std::uint8_t>(image.begin(), image.begin() + length));
}

TEST_CASE("a lost chunk repeats the expected offset and go-back-N recovers")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const std::uint32_t length = CHUNK_BYTES;

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, 2U * length));

    SECTION("a chunk that jumps ahead of the hole is dropped")
    {
        const auto ahead = chunkPacket(TEST_SESSION, 3U * length, &image[3U * length], length);
        REQUIRE(decodeChunkAck(feed(updater, ahead.data(), ahead.size(), in)).nextOffset ==
                2U * length);
    }
    SECTION("an empty chunk is dropped")
    {
        const auto empty = chunkPacket(TEST_SESSION, 2U * length, image.data(), 0U);
        REQUIRE(decodeChunkAck(feed(updater, empty.data(), empty.size(), in)).nextOffset ==
                2U * length);
    }
    SECTION("a chunk running past the announced image is dropped")
    {
        const auto tail = chunkPacket(TEST_SESSION, TEST_IMAGE_SIZE - 4U, image.data(), length);
        REQUIRE(decodeChunkAck(feed(updater, tail.data(), tail.size(), in)).nextOffset ==
                2U * length);
    }
    SECTION("a chunk of another session is dropped in silence")
    {
        const auto stale = chunkPacket(OTHER_SESSION, 2U * length, &image[2U * length], length);
        const Reply reply = feed(updater, stale.data(), stale.size(), in);
        REQUIRE(reply.consumed);
        REQUIRE(reply.bytes.empty());
    }

    // Whatever was dropped, resuming at the expected offset completes the
    // transfer and the image lands byte for byte.
    std::uint32_t offset = 2U * length;
    while (offset < TEST_IMAGE_SIZE)
    {
        const std::uint32_t step = std::min<std::uint32_t>(length, TEST_IMAGE_SIZE - offset);
        const auto packet = chunkPacket(TEST_SESSION, offset, &image[offset], step);
        static_cast<void>(feed(updater, packet.data(), packet.size(), in));
        offset += step;
    }

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    REQUIRE(store.slotBytes(mark4::OTA_SLOT_B, 0U, TEST_IMAGE_SIZE) == image);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
}

TEST_CASE("a finish with bytes missing answers the chunk acknowledgement")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const std::uint32_t sent = 3U * CHUNK_BYTES;

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, sent));

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    const Reply reply = feed(updater, finish.data(), finish.size(), in);
    REQUIRE(decodeChunkAck(reply).nextOffset == sent);
    REQUIRE(updater.sessionActive()); // the tail can still be sent
    REQUIRE(store.metaWriteCount() == 1U);
}

TEST_CASE("a transfer that does not match its announced CRC is never staged")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image) ^ 0x01U);
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    const Ack ack = decodeAck(feed(updater, finish.data(), finish.size(), in));
    REQUIRE(ack.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_FINISH));
    REQUIRE(ack.result == mark4::OTA_RESULT_CRC_MISMATCH);
    REQUIRE(!updater.sessionActive());

    // The slot holds the bytes but no record vouches for them: the bootloader
    // will not look at it, and the running firmware is untouched.
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(store.metaWriteCount() == 1U);
}

TEST_CASE("an image header the board disagrees with is never staged")
{
    ImageSpec spec;
    std::uint8_t running = mark4::OTA_SLOT_A;

    SECTION("built for another chip")
    {
        spec.mcuId = mark4::OTA_MCU_STM32F405;
    }
    SECTION("linked for the other slot")
    {
        spec.slotId = mark4::OTA_SLOT_A;
    }
    SECTION("linked for the slot that is running")
    {
        // Running from B, so the transfer targets A; an image linked for B
        // must be refused even though it is a perfectly good image.
        spec.slotId = mark4::OTA_SLOT_B;
        running = mark4::OTA_SLOT_B;
    }
    SECTION("claiming another size than the one announced")
    {
        spec.declaredSize = TEST_IMAGE_SIZE - 4U;
    }
    SECTION("no magic at all")
    {
        spec.magic = 0U;
    }
    SECTION("a header layout from another era")
    {
        spec.headerVersion = mark4::OTA_IMAGE_HEADER_VERSION + 1U;
    }

    FakeStore store(running, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(spec);

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_BAD_IMAGE);
    REQUIRE(!updater.sessionActive());
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_A] != mark4::OTA_SLOT_STAGED);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] != mark4::OTA_SLOT_STAGED);
}

TEST_CASE("an update is refused when it must not even start")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));

    SECTION("while armed")
    {
        mark4::OtaUpdater::Inputs armed;
        armed.armed = true;
        const Ack ack = decodeAck(feed(updater, begin.data(), begin.size(), armed));
        REQUIRE(ack.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_BEGIN));
        REQUIRE(ack.result == mark4::OTA_RESULT_DENIED_ARMED);
    }
    SECTION("under the voltage floor")
    {
        mark4::OtaUpdater::Inputs empty;
        empty.voltageOk = false;
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), empty)).result ==
                mark4::OTA_RESULT_DENIED_VOLTAGE);
    }
    SECTION("an image smaller than its own header")
    {
        const mark4::OtaUpdater::Inputs in;
        const auto tiny = beginPacket(TEST_SESSION, mark4::OTA_IMAGE_HEADER_SIZE - 1U, 0U);
        REQUIRE(decodeAck(feed(updater, tiny.data(), tiny.size(), in)).result ==
                mark4::OTA_RESULT_BAD_IMAGE);
    }
    SECTION("an image larger than the slot")
    {
        const mark4::OtaUpdater::Inputs in;
        const auto huge = beginPacket(TEST_SESSION, TEST_SLOT_SIZE + 1U, 0U);
        REQUIRE(decodeAck(feed(updater, huge.data(), huge.size(), in)).result ==
                mark4::OTA_RESULT_BAD_IMAGE);
    }
    SECTION("a slot that cannot be erased")
    {
        const mark4::OtaUpdater::Inputs in;
        store.failErases();
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
                mark4::OTA_RESULT_STORE_FAILURE);
    }
    SECTION("a metadata log that cannot be written")
    {
        const mark4::OtaUpdater::Inputs in;
        store.allowMetaWrites(0U);
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
                mark4::OTA_RESULT_STORE_FAILURE);
        REQUIRE(store.eraseCount(mark4::OTA_SLOT_B) == 0U); // nothing was wiped for nothing
    }

    REQUIRE(!updater.sessionActive());
    REQUIRE(store.slotBytes(mark4::OTA_SLOT_B, 0U, 8U) == std::vector<std::uint8_t>(8U, 0xFFU));
}

TEST_CASE("a second session is refused, but the same one may be restarted")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const std::uint32_t length = CHUNK_BYTES;

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, 2U * length));

    SECTION("another nonce while one session is open")
    {
        const auto other = beginPacket(OTHER_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
        REQUIRE(decodeAck(feed(updater, other.data(), other.size(), in)).result ==
                mark4::OTA_RESULT_DENIED_BUSY);
        // The open session survives untouched: the next chunk still lands.
        const auto next = chunkPacket(TEST_SESSION, 2U * length, &image[2U * length], length);
        const Reply reply = feed(updater, next.data(), next.size(), in);
        REQUIRE(reply.consumed);
        REQUIRE(reply.bytes.empty());
        REQUIRE(store.eraseCount(mark4::OTA_SLOT_B) == 1U);
    }
    SECTION("the same nonce restarts the transfer from zero")
    {
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).session == TEST_SESSION);
        REQUIRE(store.eraseCount(mark4::OTA_SLOT_B) == 2U);
        REQUIRE(updater.sessionActive());

        // A chunk at the offset that was already written is now out of order:
        // the restart really rewound the session.
        const auto stale = chunkPacket(TEST_SESSION, 2U * length, &image[2U * length], length);
        REQUIRE(decodeChunkAck(feed(updater, stale.data(), stale.size(), in)).nextOffset == 0U);

        static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));
        const auto finish =
            sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
        REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
                mark4::OTA_RESULT_OK);
    }
}

TEST_CASE("an abort drops the session and only its own session")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto stray =
        sessionPacket<mark4::OtaAbortPacket>(mark4::PacketType::OTA_ABORT, TEST_SESSION);
    const Ack strayAck = decodeAck(feed(updater, stray.data(), stray.size(), in));
    REQUIRE(strayAck.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_ABORT));
    REQUIRE(strayAck.result == mark4::OTA_RESULT_BAD_SESSION);

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, CHUNK_BYTES));

    const auto wrong =
        sessionPacket<mark4::OtaAbortPacket>(mark4::PacketType::OTA_ABORT, OTHER_SESSION);
    REQUIRE(decodeAck(feed(updater, wrong.data(), wrong.size(), in)).result ==
            mark4::OTA_RESULT_BAD_SESSION);
    REQUIRE(updater.sessionActive());

    const Ack ack = decodeAck(feed(updater, stray.data(), stray.size(), in));
    REQUIRE(ack.result == mark4::OTA_RESULT_OK);
    REQUIRE(ack.session == TEST_SESSION);
    REQUIRE(!updater.sessionActive());

    // The half-written slot is left as it is, and nothing vouches for it.
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(store.metaWriteCount() == 1U);

    // A finish for the session that no longer exists is refused, not obeyed.
    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_BAD_SESSION);
}

TEST_CASE("a session whose sender goes silent is dropped")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    mark4::OtaUpdater::Inputs in;
    in.nowUs = 1000U;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);

    updater.tick(in.nowUs + mark4::OtaUpdater::SESSION_TIMEOUT_US - 1U);
    REQUIRE(updater.sessionActive());

    // A chunk pushes the deadline back: only silence closes a session.
    in.nowUs += mark4::OtaUpdater::SESSION_TIMEOUT_US - 1U;
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, CHUNK_BYTES));
    updater.tick(in.nowUs + mark4::OtaUpdater::SESSION_TIMEOUT_US - 1U);
    REQUIRE(updater.sessionActive());

    updater.tick(in.nowUs + mark4::OtaUpdater::SESSION_TIMEOUT_US);
    REQUIRE(!updater.sessionActive());

    // Dropped in silence: there is nobody left to answer, and the slot keeps
    // whatever it holds without a record vouching for it.
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_BAD_SESSION);
}

TEST_CASE("a session opened before its own erase survives the frozen clock")
{
    // On the board the begin's timestamp is sampled before handle() spends
    // seconds erasing the slot with the core frozen: the first timeout
    // sweep then runs long past the stamp. It must re-arm the deadline,
    // not judge the fresh session against time the erase itself consumed
    // (the first bench transfer died exactly this way).
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    mark4::OtaUpdater::Inputs in;
    in.nowUs = 1000U;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);

    // First sweep far beyond the timeout: the erase ate that time.
    const std::uint64_t afterErase = in.nowUs + (2U * mark4::OtaUpdater::SESSION_TIMEOUT_US);
    updater.tick(afterErase);
    REQUIRE(updater.sessionActive());

    // The re-armed deadline counts from that sweep, and real silence
    // still closes the session.
    updater.tick(afterErase + mark4::OtaUpdater::SESSION_TIMEOUT_US - 1U);
    REQUIRE(updater.sessionActive());
    updater.tick(afterErase + mark4::OtaUpdater::SESSION_TIMEOUT_US);
    REQUIRE(!updater.sessionActive());
}

TEST_CASE("a store that dies mid-transfer takes the session down with it")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const std::uint32_t length = CHUNK_BYTES;

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, 2U * length));

    store.failProgramFrom(2U * length);
    const auto packet = chunkPacket(TEST_SESSION, 2U * length, &image[2U * length], length);
    const Ack ack = decodeAck(feed(updater, packet.data(), packet.size(), in));
    REQUIRE(ack.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_CHUNK));
    REQUIRE(ack.result == mark4::OTA_RESULT_STORE_FAILURE);
    REQUIRE(ack.session == TEST_SESSION);
    REQUIRE(!updater.sessionActive());
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
}

TEST_CASE("a metadata log that fails at staging keeps the session open")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    store.allowMetaWrites(1U); // the begin record goes through, the staging one does not
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));

    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_STORE_FAILURE);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);

    // Every byte is in the slot and verified, so the session stays open and a
    // retried finish stages it once the store cooperates.
    REQUIRE(updater.sessionActive());
    store.allowMetaWrites(1U);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
    REQUIRE(!updater.sessionActive());
}

TEST_CASE("only a trial boot can be confirmed")
{
    FakeStore store(mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const auto confirm = barePacket(mark4::PacketType::OTA_CONFIRM);

    SECTION("a running slot nobody staged")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_A;
        meta.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_EMPTY};
        store.forceMeta(meta);

        const Ack ack = decodeAck(feed(updater, confirm.data(), confirm.size(), in));
        REQUIRE(ack.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_CONFIRM));
        REQUIRE(ack.result == mark4::OTA_RESULT_BAD_STATE);
        REQUIRE(ack.session == 0U);
        REQUIRE(store.metaWriteCount() == 0U);
    }
    SECTION("a slot that is only staged, not yet tried")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_A;
        meta.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED};
        store.forceMeta(meta);

        REQUIRE(decodeAck(feed(updater, confirm.data(), confirm.size(), in)).result ==
                mark4::OTA_RESULT_BAD_STATE);
    }
    SECTION("a trial in progress")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_A;
        meta.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};
        meta.trialAttempted = true;
        store.forceMeta(meta);

        REQUIRE(decodeAck(feed(updater, confirm.data(), confirm.size(), in)).result ==
                mark4::OTA_RESULT_OK);
        REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
        REQUIRE(store.meta().activeSlot == mark4::OTA_SLOT_B);
        REQUIRE(!store.meta().trialAttempted);
        // The image that was running before stays exactly as trusted as it
        // was: that is the rollback target.
        REQUIRE(store.meta().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    }
    SECTION("a metadata log that cannot be written")
    {
        mark4::OtaMetaState meta;
        meta.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};
        store.forceMeta(meta);
        store.allowMetaWrites(0U);

        REQUIRE(decodeAck(feed(updater, confirm.data(), confirm.size(), in)).result ==
                mark4::OTA_RESULT_STORE_FAILURE);
    }
    SECTION("a metadata log that cannot be read")
    {
        store.failMetaReads();
        REQUIRE(decodeAck(feed(updater, confirm.data(), confirm.size(), in)).result ==
                mark4::OTA_RESULT_STORE_FAILURE);
    }
}

TEST_CASE("a revert needs a valid image to fall back to")
{
    FakeStore store(mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const auto revert = barePacket(mark4::PacketType::OTA_REVERT);

    SECTION("the other slot holds nothing")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_B;
        meta.slotState = {mark4::OTA_SLOT_EMPTY, mark4::OTA_SLOT_VALID};
        store.forceMeta(meta);

        const Ack ack = decodeAck(feed(updater, revert.data(), revert.size(), in));
        REQUIRE(ack.ackedType == static_cast<std::uint8_t>(mark4::PacketType::OTA_REVERT));
        REQUIRE(ack.result == mark4::OTA_RESULT_BAD_STATE);
        REQUIRE(store.metaWriteCount() == 0U);
    }
    SECTION("the other slot went bad")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_B;
        meta.slotState = {mark4::OTA_SLOT_BAD, mark4::OTA_SLOT_VALID};
        store.forceMeta(meta);

        REQUIRE(decodeAck(feed(updater, revert.data(), revert.size(), in)).result ==
                mark4::OTA_RESULT_BAD_STATE);
    }
    SECTION("the other slot is the previous firmware")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_B;
        meta.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_VALID};
        store.forceMeta(meta);

        REQUIRE(decodeAck(feed(updater, revert.data(), revert.size(), in)).result ==
                mark4::OTA_RESULT_OK);
        REQUIRE(store.meta().activeSlot == mark4::OTA_SLOT_A);
        // Nothing else moves: the reboot that follows is what changes what
        // runs, and the slot states are still the truth about both images.
        REQUIRE(store.meta().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
        REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    }
}

TEST_CASE("the status packet says what runs, from where, and what is staged")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const auto request = barePacket(mark4::PacketType::OTA_STATUS_REQUEST);

    SECTION("a slot with no image header reports no version at all")
    {
        const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
        REQUIRE(status.mcuId == mark4::OTA_MCU_SIM);
        REQUIRE(status.runningSlot == mark4::OTA_SLOT_A);
        REQUIRE(status.slotSize == TEST_SLOT_SIZE);
        REQUIRE(status.maxChunkData == mark4::OTA_CHUNK_DATA_SIZE);
        REQUIRE(status.updaterBusy == 0U);
        REQUIRE(status.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
        REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
        REQUIRE(status.versionMajor == 0U);
        REQUIRE(status.versionMinor == 0U);
        REQUIRE(status.versionPatch == 0U);
        REQUIRE(status.gitHash == std::array<char, mark4::OTA_GIT_HASH_SIZE>{});
    }
    SECTION("an image that was never packaged still reports its version")
    {
        ImageSpec spec;
        spec.slotId = mark4::OTA_SLOT_A;
        spec.stamped = false; // an elf flashed over SWD, no CRC stamped
        const std::vector<std::uint8_t> image = buildImage(spec);
        store.poke(mark4::OTA_SLOT_A, 0U, image.data(), image.size());

        const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
        REQUIRE(status.versionMajor == 1U);
        REQUIRE(status.versionMinor == 2U);
        REQUIRE(status.versionPatch == 3U);
        const std::array<char, mark4::OTA_GIT_HASH_SIZE> hash = {
            'd', 'e', 'a', 'd', 'b', 'e', 'e', 'f'};
        REQUIRE(status.gitHash == hash);
    }
    SECTION("a trial boot in progress is visible in the running slot state")
    {
        mark4::OtaMetaState meta;
        meta.activeSlot = mark4::OTA_SLOT_B;
        meta.slotState = {mark4::OTA_SLOT_TESTING, mark4::OTA_SLOT_VALID};
        meta.trialAttempted = true;
        store.forceMeta(meta);

        const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
        REQUIRE(status.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_TESTING);
        REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    }
    SECTION("an open session shows the updater busy")
    {
        const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
        const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
                mark4::OTA_RESULT_OK);

        const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
        REQUIRE(status.updaterBusy == 1U);
        REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    }
    SECTION("a store that answers nothing claims nothing")
    {
        store.failMetaReads();
        store.failReads();

        const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
        REQUIRE(status.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_EMPTY);
        REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
        REQUIRE(status.versionMajor == 0U);
    }
}

TEST_CASE("what is not an OTA packet is left for the next consumer")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;

    const std::array<std::uint8_t, 4U> other = {
        mark4::PROTOCOL_VERSION, static_cast<std::uint8_t>(mark4::PacketType::RC_COMMAND), 0U, 0U};
    REQUIRE(!feed(updater, other.data(), other.size(), in).consumed);

    const std::array<std::uint8_t, 2U> wrongVersion = {
        static_cast<std::uint8_t>(mark4::PROTOCOL_VERSION + 1U),
        static_cast<std::uint8_t>(mark4::PacketType::OTA_STATUS_REQUEST)};
    REQUIRE(!feed(updater, wrongVersion.data(), wrongVersion.size(), in).consumed);

    // Every packet type, one byte short of its struct: not consumed, not
    // answered, so a truncated frame never looks like a refusal.
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});
    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(!feed(updater, begin.data(), begin.size() - 1U, in).consumed);
    const auto chunk = chunkPacket(TEST_SESSION, 0U, image.data(), 4U);
    REQUIRE(!feed(updater, chunk.data(), chunk.size() - 1U, in).consumed);
    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(!feed(updater, finish.data(), finish.size() - 1U, in).consumed);
    const auto abort =
        sessionPacket<mark4::OtaAbortPacket>(mark4::PacketType::OTA_ABORT, TEST_SESSION);
    REQUIRE(!feed(updater, abort.data(), abort.size() - 1U, in).consumed);
    const auto confirm = barePacket(mark4::PacketType::OTA_CONFIRM);
    REQUIRE(!feed(updater, confirm.data(), 1U, in).consumed);

    // And a reply buffer too small answers nothing rather than half a packet.
    const auto request = barePacket(mark4::PacketType::OTA_STATUS_REQUEST);
    std::array<std::uint8_t, 4U> tiny{};
    bool consumed = false;
    REQUIRE(updater.handle(
                request.data(), request.size(), in, tiny.data(), tiny.size(), consumed) == 0U);
    REQUIRE(consumed);
}

TEST_CASE("the trial boot round trip: staged, tried, confirmed")
{
    FakeStore store(mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    mark4::OtaUpdater updater(store);
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
    REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));
    const auto finish =
        sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
    REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    REQUIRE(store.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
    REQUIRE(store.meta().activeSlot == mark4::OTA_SLOT_A);

    // The bootloader's half of the deal: a staged slot is marked TESTING with
    // the attempt recorded, then booted once. The active slot does not move,
    // so a reset without a confirmation comes back to the old firmware.
    mark4::OtaMetaState trial = store.meta();
    trial.slotState[mark4::OTA_SLOT_B] = mark4::OTA_SLOT_TESTING;
    trial.trialAttempted = true;
    REQUIRE(store.writeMeta(trial));

    // The new firmware runs from slot B now, and says so.
    FakeStore booted(mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    booted.forceMeta(store.meta());
    booted.poke(mark4::OTA_SLOT_B, 0U, image.data(), image.size());
    mark4::OtaUpdater trialUpdater(booted);

    const auto request = barePacket(mark4::PacketType::OTA_STATUS_REQUEST);
    const Status status = decodeStatus(feed(trialUpdater, request.data(), request.size(), in));
    REQUIRE(status.runningSlot == mark4::OTA_SLOT_B);
    REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_TESTING);
    REQUIRE(status.versionMajor == 1U);

    const auto confirm = barePacket(mark4::PacketType::OTA_CONFIRM);
    REQUIRE(decodeAck(feed(trialUpdater, confirm.data(), confirm.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    REQUIRE(booted.meta().slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    REQUIRE(booted.meta().activeSlot == mark4::OTA_SLOT_B);
    REQUIRE(!booted.meta().trialAttempted);
    REQUIRE(booted.meta().slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
}

TEST_CASE("the whole flow over the file-backed store, trial boot included")
{
    const std::string directory = scratchDirectory("mark4_ota_updater_files");
    const mark4::OtaUpdater::Inputs in;
    const std::vector<std::uint8_t> image = buildImage(ImageSpec{});

    {
        mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
        REQUIRE(store.init());
        mark4::OtaUpdater updater(store);

        const auto begin = beginPacket(TEST_SESSION, TEST_IMAGE_SIZE, imageCrc(image));
        REQUIRE(decodeAck(feed(updater, begin.data(), begin.size(), in)).result ==
                mark4::OTA_RESULT_OK);
        static_cast<void>(streamImage(updater, TEST_SESSION, image, in, TEST_IMAGE_SIZE));
        const auto finish =
            sessionPacket<mark4::OtaFinishPacket>(mark4::PacketType::OTA_FINISH, TEST_SESSION);
        REQUIRE(decodeAck(feed(updater, finish.data(), finish.size(), in)).result ==
                mark4::OTA_RESULT_OK);

        // The bytes really are in the file, and the metadata really staged.
        std::vector<std::uint8_t> readBack(TEST_IMAGE_SIZE, 0U);
        REQUIRE(store.read(mark4::OTA_SLOT_B, 0U, readBack.data(), TEST_IMAGE_SIZE));
        REQUIRE(readBack == image);
        REQUIRE(store.crc32(mark4::OTA_SLOT_B, 0U, TEST_IMAGE_SIZE) == imageCrc(image));

        mark4::OtaMetaState meta;
        REQUIRE(store.readMeta(meta));
        REQUIRE(meta.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
        REQUIRE(meta.activeSlot == mark4::OTA_SLOT_A);

        // The bootloader marking the trial, through the same store.
        meta.slotState[mark4::OTA_SLOT_B] = mark4::OTA_SLOT_TESTING;
        meta.trialAttempted = true;
        REQUIRE(store.writeMeta(meta));
    }

    // A new process, running the image that was just staged: everything it
    // needs to know is in the directory.
    mark4::FirmwareStoreSim booted(directory.c_str(), mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    REQUIRE(booted.init());
    mark4::OtaUpdater updater(booted);

    const auto request = barePacket(mark4::PacketType::OTA_STATUS_REQUEST);
    const Status status = decodeStatus(feed(updater, request.data(), request.size(), in));
    REQUIRE(status.mcuId == mark4::OTA_MCU_SIM);
    REQUIRE(status.runningSlot == mark4::OTA_SLOT_B);
    REQUIRE(status.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_TESTING);
    REQUIRE(status.versionMajor == 1U);
    REQUIRE(status.versionMinor == 2U);
    REQUIRE(status.versionPatch == 3U);

    const auto confirm = barePacket(mark4::PacketType::OTA_CONFIRM);
    REQUIRE(decodeAck(feed(updater, confirm.data(), confirm.size(), in)).result ==
            mark4::OTA_RESULT_OK);

    mark4::OtaMetaState meta;
    REQUIRE(booted.readMeta(meta));
    REQUIRE(meta.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    REQUIRE(meta.activeSlot == mark4::OTA_SLOT_B);
    REQUIRE(!meta.trialAttempted);

    // The firmware that was running before is still where it was, still
    // trusted, and one revert away: the update path never touched it.
    const auto revert = barePacket(mark4::PacketType::OTA_REVERT);
    REQUIRE(decodeAck(feed(updater, revert.data(), revert.size(), in)).result ==
            mark4::OTA_RESULT_OK);
    REQUIRE(booted.readMeta(meta));
    REQUIRE(meta.activeSlot == mark4::OTA_SLOT_A);
    REQUIRE(meta.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(meta.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
}
