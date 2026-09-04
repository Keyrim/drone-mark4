#pragma once

/// @file
/// @brief AbsFirmwareStore over the ESP-IDF OTA partitions: the two
///        application slots of partitions.csv (ota_0 is slot A, ota_1 slot
///        B) and, in place of the metadata log the flight controller keeps,
///        the otadata the IDF bootloader owns. That bootloader already
///        implements the one-shot trial and the rollback of the update
///        design, so this store translates: an OtaMetaState is synthesized
///        from what IDF exposes about each partition, and every write the
///        updater makes becomes the IDF call that means the same thing (a
///        staged slot is the next boot partition, a confirmed trial cancels
///        the rollback, a revert points the bootloader at the other slot).
///
///        The image format is ESP-IDF's, not the OtaImageHeader of the F405:
///        validity is IDF's own image verification and the identity is read
///        out of the esp_app_desc_t the build stamped, whose version string
///        is "<buildEpoch>-<gitHash>".
///
///        The running slot is never erased and never programmed, and
///        program() only moves forward inside an erased slot, like the two
///        other stores: the updater relies on the store to refuse.

#include <array>
#include <cstdint>

#include "esp_partition.h"

#include "ota/firmware_store.hpp"

namespace mark4
{
    class FirmwareStoreEsp32 final : public AbsFirmwareStore
    {
      public:
        /// Bytes erased per flash call. A whole-slot erase would stall this
        /// task for seconds; between two blocks the task yields, so the idle
        /// task feeds the task watchdog and the erase costs the relay its
        /// service, not a reset.
        static constexpr std::uint32_t ERASE_BLOCK_SIZE = 65536U;

        /// Bytes read per pass when checksumming a slot.
        static constexpr std::uint32_t CRC_BLOCK_SIZE = 256U;

        FirmwareStoreEsp32() = default;

        /// @brief Finds the two OTA partitions and the one this image runs
        ///        from. The reason is logged on failure.
        /// @return false when the partition table is not the two-slot layout
        bool init();

        [[nodiscard]] std::uint8_t runningSlot() const override;
        [[nodiscard]] std::uint32_t slotSize() const override;
        [[nodiscard]] std::uint8_t mcuId() const override;

        bool eraseSlot(std::uint8_t slot) override;

        bool program(std::uint8_t slot,
                     std::uint32_t offset,
                     const std::uint8_t *data,
                     std::uint32_t size) override;

        bool read(std::uint8_t slot,
                  std::uint32_t offset,
                  std::uint8_t *dataOut,
                  std::uint32_t size) const override;

        [[nodiscard]] std::uint32_t crc32(std::uint8_t slot,
                                          std::uint32_t offset,
                                          std::uint32_t size) const override;

        /// @brief Synthesizes the metadata from otadata: the active slot is
        ///        the boot partition, each slot's state follows its IDF
        ///        state (pending-verify is TESTING, valid is VALID, invalid
        ///        or aborted is BAD, new is STAGED), a slot IDF has no
        ///        opinion about is VALID when it holds an application and
        ///        EMPTY otherwise, and the trial is attempted while the
        ///        running image is pending-verify.
        bool readMeta(OtaMetaState &stateOut) const override;

        /// @brief Applies the difference between the state as read and the
        ///        state asked for, as IDF calls: a slot newly STAGED becomes
        ///        the boot partition; the running slot newly VALID cancels
        ///        the rollback; an active slot that moved is a revert; a slot
        ///        newly EMPTY that was the boot partition hands the boot back
        ///        to the running one. Anything else is a no-op, IDF keeps no
        ///        such state and needs none.
        bool writeMeta(const OtaMetaState &state) override;

        /// @brief IDF's own image verification over the slot, plus the
        ///        length it found against the length the transfer announced.
        [[nodiscard]] bool imageValid(std::uint8_t slot, std::uint32_t imageSize) const override;

        /// @brief Build identity out of the slot's esp_app_desc_t version
        ///        string; an application built without the stamp reports
        ///        OTA_IMAGE_UNSTAMPED and an empty hash, no application at
        ///        all reports nothing.
        [[nodiscard]] bool readIdentity(std::uint8_t slot,
                                        OtaImageIdentity &identityOut) const override;

      private:
        /// @param slot slot index
        /// @return its partition, nullptr for an unknown slot or before init()
        [[nodiscard]] const esp_partition_t *partitionOf(std::uint8_t slot) const;

        /// @param partition a partition of the table
        /// @return the slot it is, OTA_SLOT_COUNT when it is neither
        [[nodiscard]] std::uint8_t slotOf(const esp_partition_t *partition) const;

        /// @param slot slot index
        /// @return true when the slot exists and is not the running one
        [[nodiscard]] bool writable(std::uint8_t slot) const;

        std::array<const esp_partition_t *, OTA_SLOT_COUNT> m_slots{}; ///< ota_0, ota_1
        std::uint8_t m_runningSlot = OTA_SLOT_A;   ///< slot this image runs from
        std::uint8_t m_writeSlot = OTA_SLOT_COUNT; ///< slot open to programming
        std::uint32_t m_writeCursor = 0U;          ///< next legal program() offset
    };
} // namespace mark4
