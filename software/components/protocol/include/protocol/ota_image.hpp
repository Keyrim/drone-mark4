#pragma once

/// @file
/// @brief The on-flash side of the firmware update, per docs/ota-design.md:
///        the image header every slot opens with, the slot and chip
///        identities the flash stores, and the transfer constants. None of
///        this is wire (the wire is mark4.proto), but it crosses process
///        boundaries all the same: the packaging script writes the header,
///        the bootloader, the updater and the hub read it.
///
///        Every CRC in this file is CRC-32/MPEG-2 (polynomial 0x04C11DB7,
///        init 0xFFFFFFFF, no reflection, no final xor), computed
///        word-wise over data padded to 4 bytes with 0xFF: the one
///        configuration the F405 hardware CRC unit can produce, so the
///        bootloader verifies an image in milliseconds and every other
///        party (F722, hub, packaging script) reproduces it in software.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "mark4.pb.h"

namespace mark4
{
    /// Firmware slot indices; a board carries exactly two.
    inline constexpr std::uint8_t OTA_SLOT_A = 0U;
    inline constexpr std::uint8_t OTA_SLOT_B = 1U;
    inline constexpr std::size_t OTA_SLOT_COUNT = 2U;

    /// Slot lifecycle states as the boot metadata persists them. EMPTY is
    /// the erased-flash byte on purpose: a freshly erased slot needs no
    /// metadata write to be in its state. The wire (mark4_OtaSlotState)
    /// numbers EMPTY 0; otaSlotStateToWire() maps between the two.
    inline constexpr std::uint8_t OTA_SLOT_STAGED = 1U;   ///< transfer complete, CRC verified
    inline constexpr std::uint8_t OTA_SLOT_TESTING = 2U;  ///< one-shot trial boot in progress
    inline constexpr std::uint8_t OTA_SLOT_VALID = 3U;    ///< confirmed, trusted to boot
    inline constexpr std::uint8_t OTA_SLOT_BAD = 4U;      ///< trial failed or CRC mismatch
    inline constexpr std::uint8_t OTA_SLOT_EMPTY = 0xFFU; ///< erased or never written

    static_assert(OTA_SLOT_STAGED == mark4_OtaSlotState_STAGED);
    static_assert(OTA_SLOT_TESTING == mark4_OtaSlotState_TESTING);
    static_assert(OTA_SLOT_VALID == mark4_OtaSlotState_VALID);
    static_assert(OTA_SLOT_BAD == mark4_OtaSlotState_BAD);

    /// @brief Maps a persisted slot state to its wire enum.
    /// @param state OTA_SLOT_* byte out of the metadata
    /// @return the wire value; anything unknown reports EMPTY
    constexpr mark4_OtaSlotState otaSlotStateToWire(std::uint8_t state)
    {
        switch (state)
        {
            case OTA_SLOT_STAGED:
            case OTA_SLOT_TESTING:
            case OTA_SLOT_VALID:
            case OTA_SLOT_BAD:
                return static_cast<mark4_OtaSlotState>(state);
            default:
                return mark4_OtaSlotState_EMPTY;
        }
    }

    /// Target chip of an image, as the image header stores it: the byte
    /// values are those of the wire's mark4_Mcu.
    inline constexpr std::uint8_t OTA_MCU_STM32F405 = mark4_Mcu_STM32F405;
    inline constexpr std::uint8_t OTA_MCU_STM32F722 = mark4_Mcu_STM32F722;
    inline constexpr std::uint8_t OTA_MCU_ESP32C3 = mark4_Mcu_ESP32C3; ///< the relay
    inline constexpr std::uint8_t OTA_MCU_SIM = mark4_Mcu_SIM;         ///< desktop flight processes

    /// Data bytes of one OtaChunk, the bound of mark4.options.
    inline constexpr std::size_t OTA_CHUNK_DATA_SIZE = sizeof(mark4_OtaChunk_data_t::bytes);

    /// The board acknowledges every this-many in-order chunks (and any
    /// out-of-order one immediately); the sender keeps at most one window
    /// in flight, which is also the flow control protecting the relay's
    /// UART transmit side.
    inline constexpr std::uint32_t OTA_CHUNK_ACK_WINDOW = 16U;

    /// Characters of the git hash carried by images and status: ASCII
    /// hex, zero-padded; all 0xFF in an image never packaged (an elf
    /// flashed over SWD during development).
    inline constexpr std::size_t OTA_GIT_HASH_SIZE = 8U;

    /// First bytes of an image: "M4FW" read as a little-endian word.
    inline constexpr std::uint32_t OTA_IMAGE_MAGIC = 0x5746344DU;

    /// Layout revision of OtaImageHeader itself.
    inline constexpr std::uint16_t OTA_IMAGE_HEADER_VERSION = 1U;

    /// Bytes reserved for the header at the base of every slot; the
    /// vector table follows it, which keeps the table 512-byte aligned.
    inline constexpr std::size_t OTA_IMAGE_HEADER_SIZE = 512U;

    /// The imageCrc / imageSize / buildEpoch / gitHash placeholder of an
    /// image that was linked but never packaged (an elf flashed over SWD
    /// during development): erased-flash bytes. The bootloader boots such
    /// an image without a CRC check; the OTA path always stamps real values
    /// (scripts/make_ota.py), so every image that travelled the wire is
    /// verified on every boot.
    inline constexpr std::uint32_t OTA_IMAGE_UNSTAMPED = 0xFFFFFFFFU;

    /// Bytes of the header left unused, which is what makes OtaImageHeader
    /// exactly OTA_IMAGE_HEADER_SIZE long: room for a signature later.
    inline constexpr std::size_t OTA_IMAGE_RESERVED_SIZE = 480U;

    /// The byte an erased flash cell reads as: what every field the
    /// packaging script owns starts out as, the git hash included.
    inline constexpr std::uint8_t OTA_ERASED_BYTE = 0xFFU;

#pragma pack(push, 1)
    /// The first OTA_IMAGE_HEADER_SIZE bytes of every firmware slot. The
    /// constant fields (magic through slotId) are filled at compile time
    /// by the firmware itself; imageSize, imageCrc, buildEpoch and
    /// gitHash are stamped into the binary by the packaging script.
    /// Little-endian, packed: read it with memcpy, never through a
    /// reference to a member.
    struct OtaImageHeader
    {
        std::uint32_t magic;         ///< = OTA_IMAGE_MAGIC
        std::uint16_t headerVersion; ///< = OTA_IMAGE_HEADER_VERSION
        std::uint8_t mcuId;          ///< OTA_MCU_* this image was built for
        std::uint8_t slotId;         ///< slot this image was linked for
        std::uint32_t imageSize;     ///< total image bytes, header included
        std::uint32_t imageCrc;      ///< CRC-32/MPEG-2 of the bytes after the header
        std::uint32_t buildEpoch;    ///< packaging time [unix s], the build's identity:
                                     ///< every packaging run gets its own value, so two
                                     ///< builds of the same dirty tree stay distinguishable
        std::array<char, OTA_GIT_HASH_SIZE> gitHash; ///< short hash, stamped at packaging
        std::array<std::uint8_t, OTA_IMAGE_RESERVED_SIZE>
            reserved;            ///< 0xFF, room to grow (signing)
        std::uint32_t headerCrc; ///< CRC-32/MPEG-2 of the 508 bytes above, stamped at packaging
    };
#pragma pack(pop)

    static_assert(sizeof(OtaImageHeader) == OTA_IMAGE_HEADER_SIZE,
                  "the vector table alignment depends on this size");
    static_assert(std::is_trivially_copyable_v<OtaImageHeader>);
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(OtaImageHeader, headerVersion) == 4U);
    static_assert(offsetof(OtaImageHeader, mcuId) == 6U);
    static_assert(offsetof(OtaImageHeader, slotId) == 7U);
    static_assert(offsetof(OtaImageHeader, imageSize) == 8U);
    static_assert(offsetof(OtaImageHeader, imageCrc) == 12U);
    static_assert(offsetof(OtaImageHeader, buildEpoch) == 16U);
    static_assert(offsetof(OtaImageHeader, gitHash) == 20U);
    static_assert(offsetof(OtaImageHeader, headerCrc) == 508U);
    // NOLINTEND(readability-magic-numbers)

    /// @brief Copies an image git hash into a wire string field: bounded,
    ///        and empty when the image was never stamped (0xFF bytes).
    /// @param hash hash field, already copied out of the packed header
    /// @param[out] out wire field, OTA_GIT_HASH_SIZE + 1 chars
    inline void otaGitHashToWire(const std::array<char, OTA_GIT_HASH_SIZE> &hash,
                                 char (&out)[OTA_GIT_HASH_SIZE + 1U])
    {
        std::size_t length = 0U;
        while (length < hash.size() && hash[length] != '\0' &&
               static_cast<std::uint8_t>(hash[length]) != OTA_ERASED_BYTE)
        {
            out[length] = hash[length];
            ++length;
        }
        out[length] = '\0';
    }
} // namespace mark4
