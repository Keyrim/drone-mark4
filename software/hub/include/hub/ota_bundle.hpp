#pragma once

/// @file
/// @brief The .ota bundle a firmware build produces, read and validated on
///        the ground side. One file holds the manifest (what firmware this
///        is, for which chip) and one complete image per firmware slot; the
///        hub sends the image whose slot matches the INACTIVE slot of the
///        board, so nothing in the transfer path ever touches the running
///        one.
///
///        Layout, little-endian throughout:
///          8   bytes  magic "M4OTA1\0\0"
///          u32        manifest byte count
///          N   bytes  manifest, ASCII JSON
///          then per slot, in slot order:
///            u32      image byte count
///            M bytes  image, starting with the 512-byte OtaImageHeader
///
///        Manifest: {"name", "mcuId", "version":{"major","minor","patch"},
///        "gitHash", "protocolVersion", "images":[{"slot","size","crc32"}]}.
///        Loading validates every cross-check it can make on its own: the
///        magic, the protocol version against PROTOCOL_VERSION, the
///        announced sizes against the bytes actually present, the announced
///        CRC against the bytes actually present, and the image header of
///        each image against the manifest entry that describes it. A bundle
///        that loads is therefore internally consistent; only the match
///        against the board (mcu, slot, slot size) is left to the session.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/ota.hpp"

namespace mark4
{
    /// First bytes of every bundle file.
    inline constexpr std::size_t OTA_BUNDLE_MAGIC_SIZE = 8U;

    /// @return the magic every bundle opens with, "M4OTA1" and two zeros
    [[nodiscard]] const char *otaBundleMagic();

    /// CRC-32/MPEG-2 of a byte range, tail padded to a word with 0xFF: the
    /// one checksum of the update system (see protocol/ota.hpp). The hub
    /// links protocol/ headers alone, so it carries its own copy of this
    /// rather than reaching into platform_common for it.
    /// @param data bytes to checksum
    /// @param size byte count
    /// @return the CRC the board and the packaging script compute too
    [[nodiscard]] std::uint32_t otaImageCrc32(const std::uint8_t *data, std::size_t size);

    /// One firmware image of a bundle: the manifest entry and the bytes.
    struct OtaBundleImage
    {
        std::uint8_t slot = OTA_SLOT_A;  ///< slot this image is linked for
        std::uint32_t size = 0U;         ///< image bytes, header included
        std::uint32_t crc32 = 0U;        ///< CRC-32/MPEG-2 of the whole image
        std::vector<std::uint8_t> bytes; ///< the image itself, size long
    };

    /// A loaded bundle: what the manifest says, and every image it holds.
    struct OtaBundle
    {
        std::string path;                   ///< file it was read from
        std::string name;                   ///< firmware name, for the operator
        std::uint8_t mcuId = 0U;            ///< OTA_MCU_* this build targets
        std::uint8_t versionMajor = 0U;     ///< firmware version
        std::uint8_t versionMinor = 0U;     ///< firmware version
        std::uint8_t versionPatch = 0U;     ///< firmware version
        std::string gitHash;                ///< short hash of the build
        std::uint8_t protocolVersion = 0U;  ///< wire version the build speaks
        std::vector<OtaBundleImage> images; ///< one entry per slot, in slot order

        /// @return true when a bundle was actually loaded into this object
        [[nodiscard]] bool loaded() const
        {
            return !images.empty();
        }
    };

    /// @brief Reads and validates one bundle file.
    /// @param path bundle file to read
    /// @param[out] bundleOut receives the bundle; left untouched on failure
    /// @param[out] errorOut receives the reason on failure, in the words the
    ///             operator reads on the page
    /// @return true when the bundle is present, well formed and consistent
    [[nodiscard]] bool loadOtaBundle(const std::string &path,
                                     OtaBundle &bundleOut,
                                     std::string &errorOut);

    /// @brief Finds the image a given slot must be filled with.
    /// @param bundle loaded bundle
    /// @param slot slot to fill, OTA_SLOT_A or OTA_SLOT_B
    /// @return the image, or nullptr when the bundle holds none for that slot
    [[nodiscard]] const OtaBundleImage *findOtaBundleImage(const OtaBundle &bundle,
                                                           std::uint8_t slot);

    /// @brief Renders a version triplet the way the pages show it.
    /// @param major major number
    /// @param minor minor number
    /// @param patch patch number
    /// @return "major.minor.patch"
    [[nodiscard]] std::string otaVersionText(std::uint8_t major,
                                             std::uint8_t minor,
                                             std::uint8_t patch);

    /// @brief Reads a git hash out of a wire or image field. The field is
    ///        zero-padded and a full-length hash carries no terminator, so
    ///        the length is bounded here rather than by strlen; an image
    ///        that was never packaged carries 0xFF bytes and comes back
    ///        empty.
    /// @param hash hash field, already copied out of the packed struct
    /// @return the hash, at most OTA_GIT_HASH_SIZE characters
    [[nodiscard]] std::string otaGitHashText(const std::array<char, OTA_GIT_HASH_SIZE> &hash);
} // namespace mark4
