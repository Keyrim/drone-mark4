#pragma once

/// @file
/// @brief The flash backend of OtaMetaLog (platform_common) on this chip:
///        the two 16 KB metadata sectors of ota_slots.hpp, addressed as
///        areas 0 and 1. It supplies the four calls the log expects and no
///        policy at all - the record format, the newest-wins rule and the
///        ping-pong belong to OtaMetaLog, which the bootloader and the
///        firmware store share so they can never disagree about the log.
///
///        Programming a record needs no preceding erase: OtaMetaLog only
///        ever writes into a record slot that still reads all 0xFF, and the
///        F405 allows programming erased cells one word at a time.
///
///        Nothing here touches the watchdog; the caller owns it.

#include <cstdint>

#include "platform_stm32/internal_flash.hpp"
#include "platform_stm32/ota_slots.hpp"

namespace mark4
{
    /// Backend contract of OtaMetaLog over the internal flash metadata
    /// sectors. Stateless, so the bootloader and the store may each hold
    /// their own.
    class OtaMetaFlashBackend
    {
      public:
        /// Bytes per metadata area, one erase sector each.
        static constexpr std::uint32_t AREA_SIZE = OTA_META_AREA_SIZE;

        /// @brief Reads bytes out of one metadata area.
        /// @param area 0 or 1
        /// @param offset byte offset inside the area
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        /// @return false when the range leaves the area
        bool read(std::uint8_t area,
                  std::uint32_t offset,
                  std::uint8_t *dataOut,
                  std::uint32_t size) const;

        /// @brief Programs bytes into one metadata area. The range must be
        ///        blank and 4-byte aligned, which every OtaMetaLog record
        ///        slot is.
        /// @param area 0 or 1
        /// @param offset byte offset inside the area
        /// @param data bytes to program
        /// @param size byte count
        /// @return false when the range leaves the area or the flash refused
        bool program(std::uint8_t area,
                     std::uint32_t offset,
                     const std::uint8_t *data,
                     std::uint32_t size);

        /// @brief Erases one whole metadata area (its sector).
        /// @param area 0 or 1
        /// @return false when the area index or the erase is invalid
        bool erase(std::uint8_t area);

      private:
        InternalFlash m_flash{}; ///< program/erase controller underneath
    };
} // namespace mark4
