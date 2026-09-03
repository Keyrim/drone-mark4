#pragma once

/// @file
/// @brief The STM32F405RG flash map of the update system (see
///        docs/ota-design.md section 4.2), shared by the bootloader, the
///        firmware store and the linker layouts. The numbers here mirror
///        the ones the stm32 variant's CMakeLists.txt feeds to
///        stm32f405_image.ld.in; the two must be changed together.
///
///        Sector geometry of the 1 MB part: sectors 0-3 are 16 KB, sector 4
///        is 64 KB, sectors 5-11 are 128 KB.
///
/// | Sectors | Bytes  | Content                                    |
/// | ------- | ------ | ------------------------------------------ |
/// | 0-1     | 32 KB  | drone_boot, SWD-flashed, never updated OTA |
/// | 2-3     | 2x16KB | boot metadata, ping-pong pair              |
/// | 4       | 64 KB  | reserved (future parameter persistence)    |
/// | 5-7     | 384 KB | slot A                                     |
/// | 8-10    | 384 KB | slot B                                     |
/// | 11      | 128 KB | spare                                      |

#include <cstdint>

#include "protocol/ota_image.hpp"

namespace mark4
{
    /// Base of the memory-mapped internal flash. Named after the driver on
    /// purpose: FLASH_BASE is a macro of the CMSIS device header.
    inline constexpr std::uint32_t INTERNAL_FLASH_BASE = 0x08000000U;

    /// Sectors of the STM32F405RG (1 MB, single bank, no bank offset in the
    /// SNB encoding).
    inline constexpr std::uint8_t FLASH_SECTOR_COUNT = 12U;

    /// Flash reserved for the bootloader: sectors 0 and 1.
    inline constexpr std::uint32_t BOOTLOADER_SIZE = 32U * 1024U;

    /// Bytes of one firmware slot: sectors 5-7 and 8-10, 3 x 128 KB.
    inline constexpr std::uint32_t OTA_SLOT_SIZE = 384U * 1024U;

    /// Sectors covered by one firmware slot.
    inline constexpr std::uint8_t OTA_SLOT_SECTOR_COUNT = 3U;

    /// Base address of slot A (sector 5).
    inline constexpr std::uint32_t OTA_SLOT_A_BASE = 0x08020000U;

    /// Base address of slot B (sector 8).
    inline constexpr std::uint32_t OTA_SLOT_B_BASE = 0x08080000U;

    /// First sector of slot A.
    inline constexpr std::uint8_t OTA_SLOT_A_FIRST_SECTOR = 5U;

    /// First sector of slot B.
    inline constexpr std::uint8_t OTA_SLOT_B_FIRST_SECTOR = 8U;

    /// The two boot metadata areas of docs/ota-design.md section 4.4, one
    /// 16 KB sector each so either can be erased without touching the other.
    inline constexpr std::uint32_t OTA_META_AREA_SIZE = 16U * 1024U;
    inline constexpr std::uint8_t OTA_META_AREA_COUNT = 2U;
    inline constexpr std::uint8_t OTA_META_AREA_0_SECTOR = 2U;
    inline constexpr std::uint8_t OTA_META_AREA_1_SECTOR = 3U;
    inline constexpr std::uint32_t OTA_META_AREA_0_BASE = 0x08008000U;
    inline constexpr std::uint32_t OTA_META_AREA_1_BASE = 0x0800C000U;

    /// Address the bootloader hands to VTOR and reads the initial stack
    /// pointer and reset vector from: the image header sits before it.
    /// @param slot OTA_SLOT_A or OTA_SLOT_B
    /// @return base address of that slot's flash window
    constexpr std::uint32_t otaSlotBase(std::uint8_t slot)
    {
        return slot == OTA_SLOT_B ? OTA_SLOT_B_BASE : OTA_SLOT_A_BASE;
    }

    /// @brief First erase sector of a slot.
    /// @param slot OTA_SLOT_A or OTA_SLOT_B
    constexpr std::uint8_t otaSlotFirstSector(std::uint8_t slot)
    {
        return slot == OTA_SLOT_B ? OTA_SLOT_B_FIRST_SECTOR : OTA_SLOT_A_FIRST_SECTOR;
    }

    /// @brief The slot that is not the given one.
    /// @param slot OTA_SLOT_A or OTA_SLOT_B
    constexpr std::uint8_t otaOtherSlot(std::uint8_t slot)
    {
        return slot == OTA_SLOT_A ? OTA_SLOT_B : OTA_SLOT_A;
    }

    /// @brief Erase sector holding a metadata area.
    /// @param area 0 or 1
    constexpr std::uint8_t otaMetaAreaSector(std::uint8_t area)
    {
        return area == 0U ? OTA_META_AREA_0_SECTOR : OTA_META_AREA_1_SECTOR;
    }

    /// @brief Base address of a metadata area.
    /// @param area 0 or 1
    constexpr std::uint32_t otaMetaAreaBase(std::uint8_t area)
    {
        return area == 0U ? OTA_META_AREA_0_BASE : OTA_META_AREA_1_BASE;
    }

    static_assert(OTA_SLOT_A_BASE + OTA_SLOT_SIZE <= OTA_SLOT_B_BASE,
                  "slot A must not reach into B");
    static_assert(OTA_IMAGE_HEADER_SIZE < OTA_SLOT_SIZE, "the header must leave room for code");
    static_assert(OTA_META_AREA_0_BASE >= INTERNAL_FLASH_BASE + BOOTLOADER_SIZE,
                  "the metadata must sit above the bootloader");

#ifdef DRONE_OTA_SLOT_ID
    /// The slot this translation unit is linked for. Defined only on the
    /// firmware app targets (drone_firmware_a and drone_firmware_b, one
    /// -DDRONE_OTA_SLOT_ID each): they are the only code that ends up inside
    /// a slot image, and the composition root hands this to
    /// FirmwareStoreStm32. platform_stm32 itself is compiled once for both
    /// variants and never sees the macro, which is why the store takes the
    /// running slot as a constructor argument instead of reading it here.
    inline constexpr std::uint8_t OTA_RUNNING_SLOT = DRONE_OTA_SLOT_ID;
    static_assert(OTA_RUNNING_SLOT == OTA_SLOT_A || OTA_RUNNING_SLOT == OTA_SLOT_B,
                  "DRONE_OTA_SLOT_ID must name one of the two slots");
#endif
} // namespace mark4
