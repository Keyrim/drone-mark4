/// @file
/// @brief The file-backed firmware store: it must behave like the flash it
///        stands in for, refusals included. The running slot is untouchable,
///        a slot takes bytes only after an erase and only going forward, and
///        everything nobody wrote reads 0xFF. The metadata pair survives the
///        process, which is what makes a sim board keep its slots across
///        runs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "platform/firmware_store.hpp"
#include "platform_common/crc32_mpeg2.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "protocol/ota_image.hpp"

namespace
{
    /// Slot size of these tests: large enough for a header-sized write, small
    /// enough that the backing files stay a few kilobytes.
    constexpr std::uint32_t TEST_SLOT_SIZE = 2048U;

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

    /// @brief Reads a slot range into a vector.
    /// @param store store to read from
    /// @param slot slot to read
    /// @param offset byte offset from the slot base
    /// @param size byte count
    /// @return the bytes read
    std::vector<std::uint8_t> readSlot(const mark4::FirmwareStoreSim &store,
                                       std::uint8_t slot,
                                       std::uint32_t offset,
                                       std::uint32_t size)
    {
        std::vector<std::uint8_t> bytes(size, 0U);
        REQUIRE(store.read(slot, offset, bytes.data(), size));
        return bytes;
    }
} // namespace

TEST_CASE("a fresh run directory is two blank slots and a blank metadata log")
{
    const std::string directory = scratchDirectory("mark4_ota_store_fresh");
    mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    REQUIRE(store.init());

    REQUIRE(store.mcuId() == mark4::OTA_MCU_SIM);
    REQUIRE(store.runningSlot() == mark4::OTA_SLOT_A);
    REQUIRE(store.slotSize() == TEST_SLOT_SIZE);
    REQUIRE(std::filesystem::exists(store.slotPath(mark4::OTA_SLOT_A)));
    REQUIRE(std::filesystem::exists(store.slotPath(mark4::OTA_SLOT_B)));
    REQUIRE(std::filesystem::exists(store.metaPath(0U)));
    REQUIRE(std::filesystem::exists(store.metaPath(1U)));

    const std::vector<std::uint8_t> blank(TEST_SLOT_SIZE, 0xFFU);
    REQUIRE(readSlot(store, mark4::OTA_SLOT_A, 0U, TEST_SLOT_SIZE) == blank);
    REQUIRE(readSlot(store, mark4::OTA_SLOT_B, 0U, TEST_SLOT_SIZE) == blank);

    mark4::OtaMetaState meta;
    REQUIRE(store.readMeta(meta));
    REQUIRE(meta.activeSlot == mark4::OTA_SLOT_A);
    REQUIRE(meta.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(meta.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(!meta.trialAttempted);
}

TEST_CASE("the running slot refuses every write, whatever the caller asks")
{
    const std::string directory = scratchDirectory("mark4_ota_store_running");
    mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    REQUIRE(store.init());

    const std::array<std::uint8_t, 4U> data = {1U, 2U, 3U, 4U};
    REQUIRE(!store.eraseSlot(mark4::OTA_SLOT_B));
    REQUIRE(!store.program(mark4::OTA_SLOT_B, 0U, data.data(), data.size()));
    REQUIRE(!store.eraseSlot(static_cast<std::uint8_t>(mark4::OTA_SLOT_COUNT))); // no such slot
    REQUIRE(!store.program(
        static_cast<std::uint8_t>(mark4::OTA_SLOT_COUNT), 0U, data.data(), data.size()));

    // The other slot takes them, so the refusal is about the running slot and
    // nothing else.
    REQUIRE(store.eraseSlot(mark4::OTA_SLOT_A));
    REQUIRE(store.program(mark4::OTA_SLOT_A, 0U, data.data(), data.size()));
    REQUIRE(readSlot(store, mark4::OTA_SLOT_B, 0U, 4U) ==
            std::vector<std::uint8_t>(4U, 0xFFU)); // untouched
}

TEST_CASE("a slot takes bytes only after an erase, and only going forward")
{
    const std::string directory = scratchDirectory("mark4_ota_store_order");
    const std::array<std::uint8_t, 8U> data = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U};

    {
        mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
        REQUIRE(store.init());
        REQUIRE(store.eraseSlot(mark4::OTA_SLOT_B));

        REQUIRE(store.program(mark4::OTA_SLOT_B, 0U, data.data(), 4U));
        REQUIRE(!store.program(mark4::OTA_SLOT_B, 0U, data.data(), 4U)); // same bytes twice
        REQUIRE(!store.program(mark4::OTA_SLOT_B, 2U, data.data(), 4U)); // overlapping backwards
        REQUIRE(store.program(mark4::OTA_SLOT_B, 4U, data.data() + 4U, 4U));

        // A forward jump is legal: the skipped bytes are still erased.
        REQUIRE(store.program(mark4::OTA_SLOT_B, 16U, data.data(), 4U));
        REQUIRE(!store.program(mark4::OTA_SLOT_B, 8U, data.data(), 4U));
        REQUIRE(readSlot(store, mark4::OTA_SLOT_B, 8U, 8U) == std::vector<std::uint8_t>(8U, 0xFFU));

        // Out of the slot, in every direction.
        REQUIRE(!store.program(mark4::OTA_SLOT_B, TEST_SLOT_SIZE - 2U, data.data(), 4U));
        REQUIRE(!store.program(mark4::OTA_SLOT_B, TEST_SLOT_SIZE, data.data(), 4U));
        REQUIRE(!store.program(mark4::OTA_SLOT_B, 0U, nullptr, 4U));

        std::array<std::uint8_t, 4U> out{};
        REQUIRE(!store.read(mark4::OTA_SLOT_B, TEST_SLOT_SIZE - 2U, out.data(), out.size()));
        REQUIRE(!store.read(
            static_cast<std::uint8_t>(mark4::OTA_SLOT_COUNT), 0U, out.data(), out.size()));
    }

    // A new store over the same directory has seen no erase, so the slot is
    // closed to programming until it is erased again: a half-written slot
    // cannot be extended after a restart.
    mark4::FirmwareStoreSim reopened(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    REQUIRE(reopened.init());
    REQUIRE(!reopened.program(mark4::OTA_SLOT_B, 32U, data.data(), 4U));
    REQUIRE(readSlot(reopened, mark4::OTA_SLOT_B, 0U, 8U) ==
            std::vector<std::uint8_t>(data.begin(), data.end()));

    REQUIRE(reopened.eraseSlot(mark4::OTA_SLOT_B));
    REQUIRE(readSlot(reopened, mark4::OTA_SLOT_B, 0U, 8U) == std::vector<std::uint8_t>(8U, 0xFFU));
    REQUIRE(reopened.program(mark4::OTA_SLOT_B, 0U, data.data(), 8U));
}

TEST_CASE("the store CRC is the software CRC over the same bytes, 0xFF padded")
{
    const std::string directory = scratchDirectory("mark4_ota_store_crc");
    mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
    REQUIRE(store.init());
    REQUIRE(store.eraseSlot(mark4::OTA_SLOT_B));

    // A length that is not a multiple of four: the tail padding is part of
    // the image CRC convention and must match on both sides.
    std::vector<std::uint8_t> image(70U, 0U);
    for (std::size_t i = 0U; i < image.size(); ++i)
    {
        image[i] = static_cast<std::uint8_t>(i * 7U);
    }
    REQUIRE(store.program(
        mark4::OTA_SLOT_B, 0U, image.data(), static_cast<std::uint32_t>(image.size())));

    REQUIRE(store.crc32(mark4::OTA_SLOT_B, 0U, static_cast<std::uint32_t>(image.size())) ==
            mark4::crc32Mpeg2(image.data(), image.size()));

    // A range nobody ever wrote is erased flash, checksum included.
    const std::vector<std::uint8_t> blank(64U, 0xFFU);
    REQUIRE(store.crc32(mark4::OTA_SLOT_A, 0U, 64U) ==
            mark4::crc32Mpeg2(blank.data(), blank.size()));
    REQUIRE(store.crc32(static_cast<std::uint8_t>(mark4::OTA_SLOT_COUNT), 0U, 64U) ==
            0xFFFFFFFFU); // unknown slot
}

TEST_CASE("the metadata log survives the store that wrote it")
{
    const std::string directory = scratchDirectory("mark4_ota_store_meta");
    mark4::OtaMetaState written;
    written.activeSlot = mark4::OTA_SLOT_B;
    written.slotState = {mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING};
    written.trialAttempted = true;

    {
        mark4::FirmwareStoreSim store(directory.c_str(), mark4::OTA_SLOT_A, TEST_SLOT_SIZE);
        REQUIRE(store.init());
        REQUIRE(store.writeMeta(written));

        mark4::OtaMetaState readBack;
        REQUIRE(store.readMeta(readBack));
        REQUIRE(readBack.activeSlot == written.activeSlot);
        REQUIRE(readBack.slotState == written.slotState);
        REQUIRE(readBack.trialAttempted);
    }

    mark4::FirmwareStoreSim reopened(directory.c_str(), mark4::OTA_SLOT_B, TEST_SLOT_SIZE);
    REQUIRE(reopened.init());

    mark4::OtaMetaState readBack;
    REQUIRE(reopened.readMeta(readBack));
    REQUIRE(readBack.activeSlot == mark4::OTA_SLOT_B);
    REQUIRE(readBack.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_TESTING);
    REQUIRE(readBack.trialAttempted);

    // Appending again keeps the newest record on top, across processes.
    mark4::OtaMetaState confirmed = readBack;
    confirmed.slotState[mark4::OTA_SLOT_B] = mark4::OTA_SLOT_VALID;
    confirmed.trialAttempted = false;
    REQUIRE(reopened.writeMeta(confirmed));
    REQUIRE(reopened.readMeta(readBack));
    REQUIRE(readBack.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_VALID);
    REQUIRE(!readBack.trialAttempted);
}
