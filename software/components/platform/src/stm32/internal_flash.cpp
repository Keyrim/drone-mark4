#include "platform_stm32/internal_flash.hpp"

#include <cstring>

#include <stm32f405xx.h>

#include "platform_stm32/ota_slots.hpp"

namespace mark4
{
    namespace
    {
        // RM0090 section 3.6: the two keys that release the program/erase
        // controller, written in this order to FLASH_KEYR. The device header
        // maps the registers but does not carry the keys.
        constexpr std::uint32_t FLASH_KEY1 = 0x45670123U;
        constexpr std::uint32_t FLASH_KEY2 = 0xCDEF89ABU;

        /// PSIZE = x32. The 32-bit parallelism needs a supply at or above
        /// 2.7 V (RM0090 table 7); the board runs the F405 at 3.3 V, so this
        /// is the fastest legal setting and the one word programming wants.
        constexpr std::uint32_t FLASH_CR_PSIZE_X32 = FLASH_CR_PSIZE_1;

        constexpr std::uint32_t FLASH_SR_ERRORS =
            FLASH_SR_OPERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
        constexpr std::uint32_t FLASH_SR_FLAGS = FLASH_SR_ERRORS | FLASH_SR_EOP;

        /// Sector sizes of the 1 MB part: 4 x 16 KB, 1 x 64 KB, 7 x 128 KB
        /// (RM0090 table 5). The F405RG is single bank, so the SNB field is
        /// simply the sector index, and FLASH_CR_SNB_Msk covers it; the 2 MB
        /// parts use a wider SNB encoding with a bank-2 offset, which is why
        /// the old hand-written map carried a 5-bit mask.
        constexpr std::uint32_t SECTOR_SMALL_SIZE = 16U * 1024U;
        constexpr std::uint32_t SECTOR_MEDIUM_SIZE = 64U * 1024U;
        constexpr std::uint32_t SECTOR_LARGE_SIZE = 128U * 1024U;
        constexpr std::uint8_t SECTOR_SMALL_COUNT = 4U;
        constexpr std::uint8_t SECTOR_MEDIUM_INDEX = 4U;
        constexpr std::uint8_t SECTOR_LARGE_FIRST = 5U;
        constexpr std::uint32_t SECTOR_LARGE_FIRST_BASE = 0x08020000U;

        /// Poll budget for BSY. A 128 KB sector erase is specified at up to
        /// 2 s and mass erase at 16 s (DS8626 table 45); the loop body is a
        /// handful of cycles, so this is minutes at 168 MHz and orders of
        /// magnitude beyond any legal operation. It exists only so a dead
        /// controller cannot hang the board forever.
        constexpr std::uint32_t BSY_TIMEOUT_LOOPS = 400000000U;

        /// @brief Spins until the controller reports itself idle.
        /// @return true when BSY cleared before the poll budget ran out
        bool waitNotBusy()
        {
            for (std::uint32_t loop = 0U; loop < BSY_TIMEOUT_LOOPS; ++loop)
            {
                if ((FLASH->SR & FLASH_SR_BSY) == 0U)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Clears every sticky status flag so the next operation's
        ///        error report is its own.
        void clearStatusFlags()
        {
            FLASH->SR = FLASH_SR_FLAGS;
        }

        /// @brief Turns the instruction and data caches off around a
        ///        program or erase. RM0090 section 3.5.1 requires it: the
        ///        caches do not see the controller change flash underneath
        ///        them, so a stale line would survive the write.
        /// @return the ACR cache enable bits that were on, to restore later
        std::uint32_t suspendCaches()
        {
            const std::uint32_t enabled = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
            FLASH->ACR = FLASH->ACR & ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
            return enabled;
        }

        /// @brief Flushes both caches and puts back what suspendCaches()
        ///        found on. The reset bits are only writable while the
        ///        matching cache is disabled, hence the order.
        /// @param enabled the ACR bits returned by suspendCaches()
        void resumeCaches(std::uint32_t enabled)
        {
            FLASH->ACR = FLASH->ACR | (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
            FLASH->ACR = FLASH->ACR & ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
            FLASH->ACR = FLASH->ACR | enabled;
        }

        /// @brief Reads and clears the error flags of a finished operation.
        /// @return true when the controller reported no error
        bool operationSucceeded()
        {
            const bool failed = (FLASH->SR & FLASH_SR_ERRORS) != 0U;
            clearStatusFlags();
            return !failed;
        }
    } // namespace

    std::uint32_t InternalFlash::SectorBase(std::uint8_t sector)
    {
        if (sector < SECTOR_SMALL_COUNT)
        {
            return INTERNAL_FLASH_BASE + (static_cast<std::uint32_t>(sector) * SECTOR_SMALL_SIZE);
        }
        if (sector == SECTOR_MEDIUM_INDEX)
        {
            return INTERNAL_FLASH_BASE + (SECTOR_SMALL_COUNT * SECTOR_SMALL_SIZE);
        }
        if (sector < FLASH_SECTOR_COUNT)
        {
            return SECTOR_LARGE_FIRST_BASE +
                   (static_cast<std::uint32_t>(sector - SECTOR_LARGE_FIRST) * SECTOR_LARGE_SIZE);
        }
        return INTERNAL_FLASH_BASE;
    }

    std::uint32_t InternalFlash::SectorSize(std::uint8_t sector)
    {
        if (sector < SECTOR_SMALL_COUNT)
        {
            return SECTOR_SMALL_SIZE;
        }
        if (sector == SECTOR_MEDIUM_INDEX)
        {
            return SECTOR_MEDIUM_SIZE;
        }
        if (sector < FLASH_SECTOR_COUNT)
        {
            return SECTOR_LARGE_SIZE;
        }
        return 0U;
    }

    void InternalFlash::Read(std::uint32_t address, std::uint8_t *dataOut, std::uint32_t size)
    {
        std::memcpy(dataOut, reinterpret_cast<const void *>(address), size);
    }

    bool InternalFlash::IsErased(std::uint32_t address, std::uint32_t size)
    {
        const auto *bytes = reinterpret_cast<const volatile std::uint8_t *>(address);
        for (std::uint32_t i = 0U; i < size; ++i)
        {
            if (bytes[i] != 0xFFU)
            {
                return false;
            }
        }
        return true;
    }

    bool InternalFlash::unlock()
    {
        if ((FLASH->CR & FLASH_CR_LOCK) == 0U)
        {
            return true;
        }
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
        return (FLASH->CR & FLASH_CR_LOCK) == 0U;
    }

    void InternalFlash::lock()
    {
        FLASH->CR = FLASH->CR | FLASH_CR_LOCK;
    }

    bool InternalFlash::eraseSector(std::uint8_t sector)
    {
        if (sector >= FLASH_SECTOR_COUNT)
        {
            return false;
        }
        if (!waitNotBusy() || !unlock())
        {
            return false;
        }

        const std::uint32_t caches = suspendCaches();
        clearStatusFlags();
        FLASH->CR = (FLASH->CR & ~(FLASH_CR_SNB_Msk | FLASH_CR_PG)) | FLASH_CR_PSIZE_X32 |
                    FLASH_CR_SER | (static_cast<std::uint32_t>(sector) << FLASH_CR_SNB_Pos);
        FLASH->CR = FLASH->CR | FLASH_CR_STRT;

        const bool idle = waitNotBusy();
        FLASH->CR = FLASH->CR & ~(FLASH_CR_SER | FLASH_CR_SNB_Msk);
        const bool ok = idle && operationSucceeded();
        resumeCaches(caches);
        lock();

        // A sector that still reads non-0xFF anywhere never erased, whatever
        // the status register said.
        return ok && IsErased(SectorBase(sector), SectorSize(sector));
    }

    bool InternalFlash::program(std::uint32_t address, const std::uint8_t *data, std::uint32_t size)
    {
        constexpr std::uint32_t WORD_SIZE = 4U;
        if ((address % WORD_SIZE) != 0U)
        {
            return false;
        }
        if (size == 0U)
        {
            return true;
        }
        if (!waitNotBusy() || !unlock())
        {
            return false;
        }

        const std::uint32_t caches = suspendCaches();
        clearStatusFlags();
        FLASH->CR =
            (FLASH->CR & ~(FLASH_CR_SER | FLASH_CR_SNB_Msk)) | FLASH_CR_PSIZE_X32 | FLASH_CR_PG;

        bool ok = true;
        for (std::uint32_t done = 0U; done < size && ok; done += WORD_SIZE)
        {
            // Erased flash is 0xFF, so padding a short tail with 0xFF leaves
            // those bytes exactly as an untouched cell would read.
            std::uint32_t word = 0xFFFFFFFFU;
            const std::uint32_t remaining = size - done;
            const std::uint32_t chunk = remaining < WORD_SIZE ? remaining : WORD_SIZE;
            for (std::uint32_t i = 0U; i < chunk; ++i)
            {
                word &= ~(0xFFU << (8U * i));
                word |= static_cast<std::uint32_t>(data[done + i]) << (8U * i);
            }

            auto *cell = reinterpret_cast<volatile std::uint32_t *>(address + done);
            *cell = word;
            ok = waitNotBusy() && operationSucceeded() && (*cell == word);
        }

        FLASH->CR = FLASH->CR & ~FLASH_CR_PG;
        resumeCaches(caches);
        lock();
        return ok;
    }
} // namespace mark4
