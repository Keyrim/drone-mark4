#pragma once

/// @file
/// @brief The AbsFirmwareStore image reads of the stores whose slots open
///        with the OtaImageHeader of protocol/ota_image.hpp (the F405
///        internal flash, the sim's files): whether a flashed image is
///        valid for a slot, and which build it is. Shared so the two stores
///        cannot drift apart on what a valid header is. A store over
///        another image format (the ESP32 relay over ESP-IDF partitions)
///        answers the same two questions its own way and never includes
///        this.
///
///        Board-safe: one header prefix worth of stack per call.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ota/firmware_store.hpp"
#include "protocol/ota_image.hpp"

namespace mark4
{
    /// Bytes of OtaImageHeader these reads ever need: magic through git
    /// hash. Reading the prefix instead of the whole 512-byte header keeps
    /// the stack small.
    inline constexpr std::uint32_t OTA_IMAGE_HEADER_PREFIX_SIZE = 28U;

    static_assert(OTA_IMAGE_HEADER_PREFIX_SIZE == offsetof(OtaImageHeader, reserved),
                  "the prefix must cover every field the store reads");

    /// @brief Reads the image header prefix of a slot.
    /// @param store store to read from
    /// @param slot slot to read from
    /// @param[out] headerOut header fields, valid only on true; the fields
    ///             past the prefix stay zeroed
    /// @return false on a store read failure
    [[nodiscard]] inline bool otaReadImageHeader(const AbsFirmwareStore &store,
                                                 std::uint8_t slot,
                                                 OtaImageHeader &headerOut)
    {
        std::array<std::uint8_t, OTA_IMAGE_HEADER_PREFIX_SIZE> bytes{};
        if (!store.read(slot, 0U, bytes.data(), OTA_IMAGE_HEADER_PREFIX_SIZE))
        {
            return false;
        }
        headerOut = OtaImageHeader{};
        std::memcpy(&headerOut, bytes.data(), bytes.size());
        return true;
    }

    /// @brief The imageValid() of a header-format store: the slot opens
    ///        with a header of the known layout, built for this store's
    ///        chip and linked for this very slot, announcing the size the
    ///        transfer did.
    /// @param store store the slot belongs to
    /// @param slot slot to check
    /// @param imageSize bytes the transfer announced
    /// @return false when the slot must not be staged
    [[nodiscard]] inline bool otaHeaderImageValid(const AbsFirmwareStore &store,
                                                  std::uint8_t slot,
                                                  std::uint32_t imageSize)
    {
        OtaImageHeader header{};
        return otaReadImageHeader(store, slot, header) && header.magic == OTA_IMAGE_MAGIC &&
               header.headerVersion == OTA_IMAGE_HEADER_VERSION && header.mcuId == store.mcuId() &&
               header.slotId == slot && header.imageSize == imageSize;
    }

    /// @brief The readIdentity() of a header-format store: the identity is
    ///        reported raw out of the header, stamped or not (an unstamped
    ///        image says OTA_IMAGE_UNSTAMPED and 0xFF hash bytes); a slot
    ///        with no header at all reports nothing.
    /// @param store store the slot belongs to
    /// @param slot slot to read
    /// @param[out] identityOut the identity, valid only on true
    /// @return false when the slot holds no header
    [[nodiscard]] inline bool otaHeaderIdentity(const AbsFirmwareStore &store,
                                                std::uint8_t slot,
                                                OtaImageIdentity &identityOut)
    {
        OtaImageHeader header{};
        if (!otaReadImageHeader(store, slot, header) || header.magic != OTA_IMAGE_MAGIC)
        {
            return false;
        }
        identityOut.buildEpoch = header.buildEpoch;
        std::memcpy(identityOut.gitHash.data(), &header.gitHash, OTA_GIT_HASH_SIZE);
        return true;
    }
} // namespace mark4
