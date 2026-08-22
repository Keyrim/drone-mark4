#include "platform_stm32/firmware_store_stm32.hpp"

#include "platform_common/crc32_mpeg2.hpp"
#include "platform_common/ota_meta_log.hpp"
#include "platform_stm32/ota_slots.hpp"

namespace mark4
{
    namespace
    {
        /// program() writes whole 32-bit words; the flash controller cannot
        /// do better and the wire never asks it to (chunk sizes are
        /// multiples of 4, only the last chunk of an image may be short).
        constexpr std::uint32_t PROGRAM_WORD_SIZE = 4U;

        /// Bytes hashed per pass when checksumming a slot. Small enough to
        /// leave the stack alone, large enough that the copy is noise next to
        /// the CRC arithmetic.
        constexpr std::uint32_t CRC_BLOCK_SIZE = 256U;
    } // namespace

    FirmwareStoreStm32::FirmwareStoreStm32(std::uint8_t runningSlot)
        : m_runningSlot(runningSlot == OTA_SLOT_B ? OTA_SLOT_B : OTA_SLOT_A),
          m_writeSlot(otaOtherSlot(runningSlot == OTA_SLOT_B ? OTA_SLOT_B : OTA_SLOT_A))
    {
    }

    std::uint8_t FirmwareStoreStm32::runningSlot() const
    {
        return m_runningSlot;
    }

    std::uint32_t FirmwareStoreStm32::slotSize() const
    {
        return OTA_SLOT_SIZE;
    }

    std::uint8_t FirmwareStoreStm32::mcuId() const
    {
        return OTA_MCU_STM32F405;
    }

    bool FirmwareStoreStm32::isWritableSlot(std::uint8_t slot) const
    {
        const bool known = slot == OTA_SLOT_A || slot == OTA_SLOT_B;
        return known && slot != m_runningSlot;
    }

    bool FirmwareStoreStm32::RangeFits(std::uint8_t slot, std::uint32_t offset, std::uint32_t size)
    {
        if (slot != OTA_SLOT_A && slot != OTA_SLOT_B)
        {
            return false;
        }
        return offset <= OTA_SLOT_SIZE && size <= OTA_SLOT_SIZE - offset;
    }

    bool FirmwareStoreStm32::eraseSlot(std::uint8_t slot)
    {
        if (!isWritableSlot(slot))
        {
            return false;
        }

        const std::uint8_t firstSector = otaSlotFirstSector(slot);
        for (std::uint8_t i = 0U; i < OTA_SLOT_SECTOR_COUNT; ++i)
        {
            if (!m_flash.eraseSector(static_cast<std::uint8_t>(firstSector + i)))
            {
                return false;
            }
        }

        // A fresh erase reopens the slot for writing from byte 0.
        m_writeSlot = slot;
        m_writeCursor = 0U;
        return true;
    }

    bool FirmwareStoreStm32::program(std::uint8_t slot,
                                     std::uint32_t offset,
                                     const std::uint8_t *data,
                                     std::uint32_t size)
    {
        if (!isWritableSlot(slot) || !RangeFits(slot, offset, size) || data == nullptr)
        {
            return false;
        }
        if (slot != m_writeSlot || offset != m_writeCursor)
        {
            return false; // out of order: the caller must resend from the cursor
        }
        if ((offset % PROGRAM_WORD_SIZE) != 0U)
        {
            return false;
        }

        if (!m_flash.program(otaSlotBase(slot) + offset, data, size))
        {
            return false;
        }
        m_writeCursor = offset + size;
        return true;
    }

    bool FirmwareStoreStm32::read(std::uint8_t slot,
                                  std::uint32_t offset,
                                  std::uint8_t *dataOut,
                                  std::uint32_t size) const
    {
        if (!RangeFits(slot, offset, size) || dataOut == nullptr)
        {
            return false;
        }
        InternalFlash::Read(otaSlotBase(slot) + offset, dataOut, size);
        return true;
    }

    std::uint32_t FirmwareStoreStm32::crc32(std::uint8_t slot,
                                            std::uint32_t offset,
                                            std::uint32_t size) const
    {
        // Software CRC on purpose, not the F405 hardware CRC unit. The two
        // agree bit for bit (see platform_common/crc32_mpeg2.hpp), so the
        // choice is about cost: this is one implementation shared with the
        // hub, the sim store and the desktop tests, while the hardware unit
        // would add a peripheral, an RCC dependency and a second thing to
        // keep in step for a saving of a few milliseconds per image.
        Crc32Mpeg2 crc;
        std::uint8_t block[CRC_BLOCK_SIZE];
        std::uint32_t done = 0U;
        while (done < size)
        {
            const std::uint32_t remaining = size - done;
            const std::uint32_t chunk = remaining < CRC_BLOCK_SIZE ? remaining : CRC_BLOCK_SIZE;
            if (!read(slot, offset + done, block, chunk))
            {
                return 0U;
            }
            crc.update(block, chunk);
            done += chunk;
        }
        return crc.finish();
    }

    bool FirmwareStoreStm32::readMeta(OtaMetaState &stateOut) const
    {
        OtaMetaLog<OtaMetaFlashBackend> log(m_metaBackend);
        return log.read(stateOut);
    }

    bool FirmwareStoreStm32::writeMeta(const OtaMetaState &state)
    {
        OtaMetaLog<OtaMetaFlashBackend> log(m_metaBackend);
        return log.append(state);
    }
} // namespace mark4
