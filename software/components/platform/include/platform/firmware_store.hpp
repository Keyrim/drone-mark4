#pragma once

/// @file
/// @brief Firmware slot storage behind a virtual interface, per
///        docs/ota-design.md: two execute-in-place slots, plus the boot
///        metadata that says which one runs and what each one holds. The
///        updater state machine drives this interface and never knows
///        whether the bytes land in internal flash (stm32), in files
///        (sim), or in RAM (tests).

#include <array>
#include <cstdint>

#include "protocol/ota.hpp"

namespace mark4
{
    /// The logical content of the boot metadata: the newest valid record,
    /// or the implicit state of a board that never wrote one (slot A
    /// active and trusted, nothing staged anywhere).
    struct OtaMetaState
    {
        std::uint8_t activeSlot = OTA_SLOT_A; ///< slot the bootloader prefers
        std::array<std::uint8_t, OTA_SLOT_COUNT> slotState = {OTA_SLOT_VALID,
                                                              OTA_SLOT_EMPTY}; ///< OTA_SLOT_*
        bool trialAttempted = false; ///< the TESTING slot was booted once already
    };

    /// OtaMetaRecord flags bit: the TESTING slot was booted once already.
    inline constexpr std::uint8_t OTA_META_FLAG_TRIAL_ATTEMPTED = 0x01U;

#pragma pack(push, 1)
    /// One persisted metadata record, appended to the metadata area on
    /// every state change; the newest record whose CRC verifies wins, so
    /// a torn write costs nothing. Shared verbatim by the bootloader, the
    /// stm32 store and the sim store's backing file.
    struct OtaMetaRecord
    {
        std::uint32_t counter;                              ///< monotonic, newest wins
        std::uint8_t activeSlot;                            ///< slot the bootloader prefers
        std::array<std::uint8_t, OTA_SLOT_COUNT> slotState; ///< OTA_SLOT_* per slot
        std::uint8_t flags;                                 ///< OTA_META_FLAG_* bits
        std::uint32_t reserved;                             ///< 0xFFFFFFFF
        std::uint32_t crc;                                  ///< CRC-32/MPEG-2 of the 12 bytes above
    };
#pragma pack(pop)

    /// Bytes of one OtaMetaRecord; records are appended at this stride.
    inline constexpr std::size_t OTA_META_RECORD_SIZE = 16U;

    static_assert(sizeof(OtaMetaRecord) == OTA_META_RECORD_SIZE, "record layout must be packed");
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(OtaMetaRecord, activeSlot) == 4U);
    static_assert(offsetof(OtaMetaRecord, slotState) == 5U);
    static_assert(offsetof(OtaMetaRecord, flags) == 7U);
    static_assert(offsetof(OtaMetaRecord, crc) == 12U);
    // NOLINTEND(readability-magic-numbers)

    /// Two firmware slots and the boot metadata, behind the storage
    /// details. Slots are erase-then-program-in-order devices: program()
    /// is only legal at increasing offsets after eraseSlot(), like the
    /// flash underneath. The running slot must never be erased or
    /// programmed; implementations refuse it.
    class AbsFirmwareStore
    {
      public:
        virtual ~AbsFirmwareStore() = default;

        /// @brief Slot this firmware executes from (link-time fact).
        virtual std::uint8_t runningSlot() const = 0;

        /// @brief Bytes available in each slot on this chip.
        virtual std::uint32_t slotSize() const = 0;

        /// @brief OTA_MCU_* identity of this board.
        virtual std::uint8_t mcuId() const = 0;

        /// @brief Erases one whole slot. Blocking, and seconds long on
        ///        real flash; the caller owns the watchdog around it.
        /// @param slot OTA_SLOT_A or OTA_SLOT_B, not the running slot
        /// @return false on storage failure or a refused slot
        virtual bool eraseSlot(std::uint8_t slot) = 0;

        /// @brief Programs bytes at increasing offsets into an erased slot.
        /// @param slot OTA_SLOT_A or OTA_SLOT_B, not the running slot
        /// @param offset byte offset from the slot base
        /// @param data bytes to program
        /// @param size byte count
        /// @return false on storage failure or a refused slot
        virtual bool program(std::uint8_t slot,
                             std::uint32_t offset,
                             const std::uint8_t *data,
                             std::uint32_t size) = 0;

        /// @brief Reads bytes back from a slot.
        /// @param slot OTA_SLOT_A or OTA_SLOT_B
        /// @param offset byte offset from the slot base
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        /// @return false on storage failure
        virtual bool read(std::uint8_t slot,
                          std::uint32_t offset,
                          std::uint8_t *dataOut,
                          std::uint32_t size) const = 0;

        /// @brief CRC-32/MPEG-2 of a slot range, word-wise, the range
        ///        padded to 4 bytes with 0xFF (see protocol/ota.hpp).
        /// @param slot OTA_SLOT_A or OTA_SLOT_B
        /// @param offset byte offset from the slot base
        /// @param size byte count before padding
        virtual std::uint32_t crc32(std::uint8_t slot,
                                    std::uint32_t offset,
                                    std::uint32_t size) const = 0;

        /// @brief Reads the newest valid metadata record.
        /// @param[out] stateOut filled from the record, or left at its
        ///             defaults when no record exists yet
        /// @return false only on storage failure; a blank area is true
        virtual bool readMeta(OtaMetaState &stateOut) const = 0;

        /// @brief Appends one metadata record; the store owns the
        ///        counter, the CRC and the ping-pong mechanics.
        /// @param state logical content to persist
        /// @return false on storage failure
        virtual bool writeMeta(const OtaMetaState &state) = 0;
    };
} // namespace mark4
