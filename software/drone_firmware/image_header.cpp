/// @file
/// @brief The OtaImageHeader that opens this image, laid down at the base of
///        the firmware slot by the linker (.image_header, KEEP'd so garbage
///        collection cannot drop it).
///
///        What the compiler can know it fills in: the magic, the layout
///        version, the chip and the slot this variant was linked for.
///        What only the packaging script can know it leaves at
///        OTA_IMAGE_UNSTAMPED (erased-flash bytes): the image size, the
///        image CRC, the build epoch, the git hash and the header CRC. An elf
///        flashed straight over SWD therefore boots unverified by design,
///        while every image that travelled the wire carries real checksums
///        that the bootloader checks on every single boot.
///
///        drone_boot deliberately has no counterpart of this file: it is not
///        an OTA image and its flash window starts at the reset vector.

#include <array>
#include <cstddef>
#include <cstdint>

#include "platform_stm32/ota_slots.hpp"
#include "protocol/ota.hpp"

namespace mark4
{
    namespace
    {
        /// The byte an erased flash cell reads as, and so the byte every
        /// field the packaging script owns must start out as.
        constexpr std::uint8_t ERASED_BYTE = 0xFFU;

        /// @brief Builds an array of erased-flash bytes for the fields
        ///        scripts/make_ota.py stamps and the reserved room.
        /// @return every element set to ERASED_BYTE
        template <std::size_t TSize> constexpr std::array<std::uint8_t, TSize> erasedBytes()
        {
            std::array<std::uint8_t, TSize> bytes{};
            for (std::size_t i = 0U; i < TSize; ++i)
            {
                bytes[i] = ERASED_BYTE;
            }
            return bytes;
        }

        /// @brief Same thing for the char-typed git hash field.
        /// @return every element set to ERASED_BYTE
        template <std::size_t TSize> constexpr std::array<char, TSize> erasedChars()
        {
            std::array<char, TSize> chars{};
            for (std::size_t i = 0U; i < TSize; ++i)
            {
                chars[i] = static_cast<char>(ERASED_BYTE);
            }
            return chars;
        }
    } // namespace

    /// External linkage and the used attribute: nothing in the firmware
    /// references this object, only the bootloader and the packaging script
    /// read it, out of the flash bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init)
    __attribute__((section(".image_header"), used)) const OtaImageHeader g_imageHeader = {
        .magic = OTA_IMAGE_MAGIC,
        .headerVersion = OTA_IMAGE_HEADER_VERSION,
        .mcuId = OTA_MCU_STM32F405,
        .slotId = OTA_RUNNING_SLOT,
        .imageSize = OTA_IMAGE_UNSTAMPED,
        .imageCrc = OTA_IMAGE_UNSTAMPED,
        .buildEpoch = OTA_IMAGE_UNSTAMPED,
        .gitHash = erasedChars<OTA_GIT_HASH_SIZE>(),
        .reserved = erasedBytes<sizeof(OtaImageHeader::reserved)>(),
        .headerCrc = OTA_IMAGE_UNSTAMPED,
    };
} // namespace mark4
