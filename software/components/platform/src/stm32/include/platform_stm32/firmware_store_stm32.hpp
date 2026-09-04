#pragma once

/// @file
/// @brief AbsFirmwareStore over the STM32F405 internal flash: the two
///        execute-in-place slots of ota_slots.hpp plus the boot metadata
///        log. This is the store the firmware's update mode drives; the
///        bootloader shares the flash driver and the same metadata log but
///        not this class, because it has no session to serve.
///
///        Two invariants are enforced here and nowhere else:
///          - the running slot is never erased and never programmed, which
///            is what makes a failed update unable to break the board;
///          - program() only accepts strictly increasing, word-aligned
///            offsets, because that is all the flash underneath can do.
///
///        Watchdog: the store never touches it. eraseSlot() stalls the core
///        for seconds; the caller must refresh the watchdog before calling
///        and size its window accordingly.

#include <cstdint>

#include "ota/firmware_store.hpp"
#include "platform_stm32/internal_flash.hpp"
#include "platform_stm32/ota_meta_flash.hpp"

namespace mark4
{
    class FirmwareStoreStm32 final : public AbsFirmwareStore
    {
      public:
        /// @brief Binds the store to the slot this image runs from.
        /// @param runningSlot OTA_SLOT_A or OTA_SLOT_B; a link-time fact,
        ///        which the firmware app takes from OTA_RUNNING_SLOT
        ///        (ota_slots.hpp, one -DDRONE_OTA_SLOT_ID per variant)
        explicit FirmwareStoreStm32(std::uint8_t runningSlot);

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

        bool readMeta(OtaMetaState &stateOut) const override;
        bool writeMeta(const OtaMetaState &state) override;

        /// @brief The slot opens with an OtaImageHeader for this chip and
        ///        this slot (ota/image_header.hpp).
        [[nodiscard]] bool imageValid(std::uint8_t slot, std::uint32_t imageSize) const override;

        /// @brief Build identity out of the slot's OtaImageHeader.
        [[nodiscard]] bool readIdentity(std::uint8_t slot,
                                        OtaImageIdentity &identityOut) const override;

      private:
        /// @brief Tells whether a slot may be written at all: a known slot
        ///        that is not the one this image executes from.
        /// @param slot slot index to check
        [[nodiscard]] bool isWritableSlot(std::uint8_t slot) const;

        /// @brief Tells whether a byte range fits inside a known slot.
        /// @param slot slot index to check
        /// @param offset byte offset from the slot base
        /// @param size byte count
        [[nodiscard]] static bool RangeFits(std::uint8_t slot,
                                            std::uint32_t offset,
                                            std::uint32_t size);

        std::uint8_t m_runningSlot;       ///< slot this image executes from
        std::uint32_t m_writeCursor = 0U; ///< next legal program() offset
        std::uint8_t m_writeSlot;         ///< slot m_writeCursor belongs to
        InternalFlash m_flash{};          ///< program/erase controller

        /// The two metadata sectors. Stateless, but OtaMetaLog binds a
        /// non-const reference to its backend even to read, and reading the
        /// metadata is logically const: hence mutable rather than a second
        /// backend instance living in readMeta().
        mutable OtaMetaFlashBackend m_metaBackend{};
    };
} // namespace mark4
