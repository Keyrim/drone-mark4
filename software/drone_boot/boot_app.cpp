#include "boot_app.hpp"

#include <cstring>

#include "platform_common/crc32_mpeg2.hpp"
#include "platform_common/ota_boot_policy.hpp"
#include "platform_stm32/board.hpp"
#include "platform_stm32/internal_flash.hpp"
#include "platform_stm32/ota_slots.hpp"
#include "protocol/ota_image.hpp"

namespace mark4
{
    namespace
    {
        /// Bytes hashed per pass; a block on the stack costs nothing here
        /// and keeps the CRC loop away from the flash access pattern.
        constexpr std::uint32_t CRC_BLOCK_SIZE = 256U;

        /// The main stack pointer an image announces must land in the SRAM
        /// the linker layouts hand out (128 KB at 0x20000000).
        constexpr std::uint32_t RAM_BASE = 0x20000000U;
        constexpr std::uint32_t RAM_SIZE = 128U * 1024U;

        /// "Nothing bootable" on LED1: one long flash then two short ones,
        /// then a full second dark. Deliberately unlike every firmware
        /// pattern of drone_firmware/status_leds.cpp, which are all built
        /// from equal 50 ms slots: a long flash means the firmware never
        /// started at all.
        constexpr std::uint32_t PANIC_LONG_MS = 800U;
        constexpr std::uint32_t PANIC_SHORT_MS = 120U;
        constexpr std::uint32_t PANIC_GAP_MS = 200U;
        constexpr std::uint32_t PANIC_PAUSE_MS = 1000U;
        constexpr std::uint32_t PANIC_SHORT_COUNT = 2U;

        /// @brief CRC-32/MPEG-2 of a memory-mapped flash range, per the
        ///        convention of protocol/ota.hpp.
        /// @param address absolute start address
        /// @param size byte count
        std::uint32_t crcRange(std::uint32_t address, std::uint32_t size)
        {
            Crc32Mpeg2 crc;
            std::uint8_t block[CRC_BLOCK_SIZE];
            std::uint32_t done = 0U;
            while (done < size)
            {
                const std::uint32_t remaining = size - done;
                const std::uint32_t chunk = remaining < CRC_BLOCK_SIZE ? remaining : CRC_BLOCK_SIZE;
                InternalFlash::Read(address + done, block, chunk);
                crc.update(block, chunk);
                done += chunk;
            }
            return crc.finish();
        }

    } // namespace

    bool BootApp::Validates(std::uint8_t slot)
    {
        const std::uint32_t base = otaSlotBase(slot);

        std::uint8_t bytes[OTA_IMAGE_HEADER_SIZE];
        InternalFlash::Read(base, bytes, OTA_IMAGE_HEADER_SIZE);
        OtaImageHeader header{};
        std::memcpy(&header, bytes, sizeof(header));

        if (header.magic != OTA_IMAGE_MAGIC || header.headerVersion != OTA_IMAGE_HEADER_VERSION)
        {
            return false;
        }
        if (header.mcuId != OTA_MCU_STM32F405 || header.slotId != slot)
        {
            return false;
        }

        // The vectors must be usable before anything jumps to them: a stack
        // pointer outside SRAM or an entry point outside the slot is a
        // broken image whatever its checksums say.
        std::uint32_t vectors[2] = {0U, 0U};
        InternalFlash::Read(base + OTA_IMAGE_HEADER_SIZE,
                            reinterpret_cast<std::uint8_t *>(vectors),
                            sizeof(vectors));
        if (vectors[0] <= RAM_BASE || vectors[0] > RAM_BASE + RAM_SIZE)
        {
            return false;
        }
        if (vectors[1] < base + OTA_IMAGE_HEADER_SIZE || vectors[1] >= base + OTA_SLOT_SIZE)
        {
            return false;
        }

        if (header.imageCrc == OTA_IMAGE_UNSTAMPED)
        {
            // Linked but never packaged: an elf flashed over SWD during
            // development. Nothing to check against, so trust the header.
            return true;
        }

        if (header.imageSize <= OTA_IMAGE_HEADER_SIZE || header.imageSize > OTA_SLOT_SIZE)
        {
            return false;
        }
        // The header CRC first: it is what makes imageSize and imageCrc
        // trustworthy enough to hash the code region against.
        if (header.headerCrc != crcRange(base, offsetof(OtaImageHeader, headerCrc)))
        {
            return false;
        }
        const std::uint32_t codeSize = header.imageSize - OTA_IMAGE_HEADER_SIZE;
        return header.imageCrc == crcRange(base + OTA_IMAGE_HEADER_SIZE, codeSize);
    }

    void BootApp::markBad(OtaMetaState &state, std::uint8_t slot)
    {
        state.slotState[slot] = OTA_SLOT_BAD;
        state.trialAttempted = false;
        (void)m_metaLog.append(state);
    }

    std::uint8_t BootApp::chooseSlot(OtaMetaState &state)
    {
        if (!m_metaLog.read(state))
        {
            state = OtaMetaState{};
        }

        // The decision itself is shared with the desktop flight process
        // (platform_common/ota_boot_policy.hpp): the sim's fake trial boot
        // must be the same state machine, not an imitation of it. Only the
        // storage and the image validation below are this executable's.
        const OtaBootDecision decision = otaDecideBoot(state);
        if (decision.persist)
        {
            (void)m_metaLog.append(state);
        }
        return decision.slot;
    }

    void BootApp::run()
    {
        OtaMetaState state;
        std::uint8_t slot = chooseSlot(state);
        if (slot != OTA_SLOT_A && slot != OTA_SLOT_B)
        {
            slot = OTA_SLOT_A;
        }

        if (!Validates(slot))
        {
            markBad(state, slot);
            slot = otaOtherSlot(slot);
            if (!Validates(slot))
            {
                PanicBlink();
            }
        }
        JumpToSlot(slot);
    }

    void BootApp::JumpToSlot(std::uint8_t slot)
    {
        const std::uint32_t vectorBase = otaSlotBase(slot) + OTA_IMAGE_HEADER_SIZE;
        std::uint32_t vectors[2] = {0U, 0U};
        InternalFlash::Read(vectorBase, reinterpret_cast<std::uint8_t *>(vectors), sizeof(vectors));

        __asm volatile("cpsid i" ::: "memory");
        setVectorTable(vectorBase);

        // The stack pointer and the branch must happen with nothing live: set
        // MSP and branch in one asm block so the compiler cannot slip a
        // stack access between them. The vector's low bit carries the Thumb
        // state, which is exactly what bx wants.
        __asm volatile("msr msp, %0 \n"
                       "bx %1 \n"
                       :
                       : "r"(vectors[0]), "r"(vectors[1])
                       : "memory");
        for (;;)
        {
        }
    }

    void BootApp::PanicBlink()
    {
        initCycleCounter();
        initLeds();
        for (;;)
        {
            setLed1(true);
            delayMs(PANIC_LONG_MS);
            setLed1(false);
            delayMs(PANIC_GAP_MS);
            for (std::uint32_t flash = 0U; flash < PANIC_SHORT_COUNT; ++flash)
            {
                setLed1(true);
                delayMs(PANIC_SHORT_MS);
                setLed1(false);
                delayMs(PANIC_GAP_MS);
            }
            delayMs(PANIC_PAUSE_MS);
        }
    }
} // namespace mark4
