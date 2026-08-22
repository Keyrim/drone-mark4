#pragma once

/// @file
/// @brief The STM32F405 embedded flash program/erase controller (RM0090
///        section 3.6), the only writer of internal flash on the board. It
///        knows sectors, words and the unlock sequence and nothing about
///        firmware slots: the slot geometry lives in ota_slots.hpp and the
///        update policy in FirmwareStoreStm32 and drone_boot.
///
///        Every erase and program stalls all code fetches from flash for as
///        long as the operation lasts (up to about two seconds for a 128 KB
///        sector). Nothing here touches the watchdog: the caller owns it,
///        and must refresh it before each call.

#include <cstdint>

namespace mark4
{
    /// Register-level driver for the embedded flash controller. Stateless:
    /// the flash controller itself holds all the state, so several
    /// instances (the store and the bootloader) cost nothing and cannot
    /// disagree.
    class InternalFlash
    {
      public:
        /// @brief Base address of one sector of the 1 MB part.
        /// @param sector 0 to FLASH_SECTOR_COUNT - 1
        /// @return absolute address, or FLASH_BASE for an out-of-range index
        static std::uint32_t SectorBase(std::uint8_t sector);

        /// @brief Size of one sector of the 1 MB part.
        /// @param sector 0 to FLASH_SECTOR_COUNT - 1
        /// @return byte count, or 0 for an out-of-range index
        static std::uint32_t SectorSize(std::uint8_t sector);

        /// @brief Copies bytes out of the memory-mapped flash. Reading needs
        ///        no unlock and no controller state; the helper exists so
        ///        callers never form their own pointers into flash.
        /// @param address absolute source address
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        static void Read(std::uint32_t address, std::uint8_t *dataOut, std::uint32_t size);

        /// @brief Releases the program/erase controller by writing the two
        ///        RM0090 keys. Idempotent: an already unlocked controller is
        ///        left alone. eraseSector() and program() unlock on their own
        ///        and relock before returning, so this is only for a caller
        ///        that wants the controller open across many operations.
        /// @return true when the controller is unlocked
        bool unlock();

        /// @brief Sets LOCK, so a stray write cannot reach flash.
        void lock();

        /// @brief Erases one whole sector. Blocking, and up to about two
        ///        seconds for a 128 KB sector. The flash is left locked.
        /// @param sector 0 to FLASH_SECTOR_COUNT - 1
        /// @return false on an out-of-range sector or a controller error
        bool eraseSector(std::uint8_t sector);

        /// @brief Programs bytes as 32-bit words and reads every word back
        ///        to confirm it took. The flash is left locked.
        /// @param address absolute destination, must be 4-byte aligned
        /// @param data bytes to program
        /// @param size byte count; a tail that does not fill a word is
        ///        padded with 0xFF, so a non-multiple of 4 is only legal as
        ///        the last write of a sequence
        /// @return false on a misaligned address, a controller error or a
        ///         verify mismatch
        bool program(std::uint32_t address, const std::uint8_t *data, std::uint32_t size);

        /// @brief Tells whether a range still reads as erased flash.
        /// @param address absolute start address
        /// @param size byte count
        /// @return true when every byte is 0xFF
        static bool IsErased(std::uint32_t address, std::uint32_t size);
    };
} // namespace mark4
