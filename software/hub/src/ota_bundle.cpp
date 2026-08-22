/// @file
/// @brief Bundle reader implementation. Every failure comes back as a
///        sentence the operator can act on: a bundle is a build artifact,
///        and "which of the two files did I pick" is the question a refusal
///        has to answer.

#include "hub/ota_bundle.hpp"

#include <cstring>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>

namespace mark4
{
    namespace
    {
        using Json = nlohmann::json;

        /// Bits of one CRC word, and the bytes it is fed from.
        constexpr std::size_t CRC_WORD_BITS = 32U;
        constexpr std::size_t CRC_WORD_BYTES = 4U;
        constexpr std::uint32_t CRC_POLYNOMIAL = 0x04C11DB7U;
        constexpr std::uint32_t CRC_INIT = 0xFFFFFFFFU;
        constexpr std::uint32_t CRC_TOP_BIT = 0x80000000U;
        constexpr std::uint8_t CRC_PAD_BYTE = 0xFFU;
        constexpr std::size_t BITS_PER_BYTE = 8U;

        /// Bytes of the u32 length fields the container is built from.
        constexpr std::size_t LENGTH_FIELD_SIZE = 4U;

        /// Largest manifest and largest image the reader will consider. Both
        /// are read out of the file itself, so both are bounded before a
        /// single byte is allocated: a corrupted length field must not turn
        /// into a multi-gigabyte reservation.
        constexpr std::uint32_t MAX_MANIFEST_SIZE = 64U * 1024U;
        constexpr std::uint32_t MAX_IMAGE_SIZE = 8U * 1024U * 1024U;

        /// @brief Reads a little-endian u32 out of a byte buffer.
        /// @param data buffer, at least offset + 4 bytes long
        /// @param offset byte offset of the field
        /// @return the value
        std::uint32_t readU32(const std::vector<std::uint8_t> &data, std::size_t offset)
        {
            return static_cast<std::uint32_t>(data[offset]) |
                   (static_cast<std::uint32_t>(data[offset + 1U]) << BITS_PER_BYTE) |
                   (static_cast<std::uint32_t>(data[offset + 2U]) << (2U * BITS_PER_BYTE)) |
                   (static_cast<std::uint32_t>(data[offset + 3U]) << (3U * BITS_PER_BYTE));
        }

        /// @brief Renders a 32-bit value as 0x-prefixed hex, so a CRC or a
        ///        magic in an error message reads like the constant it is
        ///        compared against.
        /// @param value value to render
        /// @return the text
        std::string hexText(std::uint32_t value)
        {
            static constexpr char DIGITS[] = "0123456789abcdef";
            static constexpr std::size_t NIBBLES = 8U;
            static constexpr std::uint32_t NIBBLE_MASK = 0x0FU;
            static constexpr std::uint32_t NIBBLE_BITS = 4U;
            std::string text = "0x";
            for (std::size_t nibble = 0U; nibble < NIBBLES; ++nibble)
            {
                const std::uint32_t shift =
                    static_cast<std::uint32_t>(NIBBLES - 1U - nibble) * NIBBLE_BITS;
                text.push_back(DIGITS[(value >> shift) & NIBBLE_MASK]);
            }
            return text;
        }

        /// @brief Reads a whole file into memory.
        /// @param path file to read
        /// @param[out] bytesOut receives the content
        /// @return true when the file was opened and read
        bool readWholeFile(const std::string &path, std::vector<std::uint8_t> &bytesOut)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0)
            {
                return false;
            }
            file.seekg(0, std::ios::beg);
            bytesOut.resize(static_cast<std::size_t>(size));
            if (!bytesOut.empty())
            {
                file.read(reinterpret_cast<char *>(bytesOut.data()),
                          static_cast<std::streamsize>(bytesOut.size()));
            }
            return file.good() || file.eof();
        }

        /// @brief Reads the mcu identity of a manifest. A number is the
        ///        OTA_MCU_* value itself; a string names the chip, which is
        ///        what a human writing a manifest by hand would put there.
        /// @param manifest manifest object
        /// @param[out] valueOut receives the identity
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the field names a known chip
        bool readMcuId(const Json &manifest, std::uint8_t &valueOut, std::string &errorOut)
        {
            const auto found = manifest.find("mcuId");
            if (found == manifest.end())
            {
                errorOut = "the manifest carries no 'mcuId'";
                return false;
            }
            if (found->is_number_unsigned())
            {
                const auto raw = found->get<std::uint64_t>();
                if (raw > UINT8_MAX)
                {
                    errorOut = "the manifest 'mcuId' is out of range";
                    return false;
                }
                valueOut = static_cast<std::uint8_t>(raw);
                return true;
            }
            if (found->is_string())
            {
                const auto name = found->get<std::string>();
                if (name == "stm32f405" || name == "f405")
                {
                    valueOut = OTA_MCU_STM32F405;
                    return true;
                }
                if (name == "stm32f722" || name == "f722")
                {
                    valueOut = OTA_MCU_STM32F722;
                    return true;
                }
                if (name == "sim")
                {
                    valueOut = OTA_MCU_SIM;
                    return true;
                }
                errorOut = "the manifest names an unknown mcu '" + name + "'";
                return false;
            }
            errorOut = "the manifest 'mcuId' must be a number or a chip name";
            return false;
        }

        /// @brief Reads one unsigned field of the manifest.
        /// @param object object to read from
        /// @param key field name
        /// @param what what the field describes, for the error message
        /// @param[out] valueOut receives the value
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the field is present and fits 32 bits
        bool readManifestU32(const Json &object,
                             const char *key,
                             const char *what,
                             std::uint32_t &valueOut,
                             std::string &errorOut)
        {
            const auto found = object.find(key);
            if (found == object.end() || !found->is_number_unsigned())
            {
                errorOut = std::string("the manifest carries no ") + what;
                return false;
            }
            const auto raw = found->get<std::uint64_t>();
            if (raw > UINT32_MAX)
            {
                errorOut = std::string("the manifest ") + what + " is out of range";
                return false;
            }
            valueOut = static_cast<std::uint32_t>(raw);
            return true;
        }

        /// @brief Reads the version triplet of the manifest.
        /// @param manifest manifest object
        /// @param[out] bundleOut receives the three numbers
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the triplet is present and in range
        bool readVersion(const Json &manifest, OtaBundle &bundleOut, std::string &errorOut)
        {
            const auto found = manifest.find("version");
            if (found == manifest.end() || !found->is_object())
            {
                errorOut = "the manifest carries no 'version' object";
                return false;
            }
            std::uint32_t major = 0U;
            std::uint32_t minor = 0U;
            std::uint32_t patch = 0U;
            if (!readManifestU32(*found, "major", "version major", major, errorOut) ||
                !readManifestU32(*found, "minor", "version minor", minor, errorOut) ||
                !readManifestU32(*found, "patch", "version patch", patch, errorOut))
            {
                return false;
            }
            if (major > UINT8_MAX || minor > UINT8_MAX || patch > UINT8_MAX)
            {
                errorOut = "the manifest version numbers must each fit a byte";
                return false;
            }
            bundleOut.versionMajor = static_cast<std::uint8_t>(major);
            bundleOut.versionMinor = static_cast<std::uint8_t>(minor);
            bundleOut.versionPatch = static_cast<std::uint8_t>(patch);
            return true;
        }

        /// @brief Reads the manifest image list, without the bytes.
        /// @param manifest manifest object
        /// @param[out] bundleOut receives one entry per listed image
        /// @param[out] errorOut receives the reason on failure
        /// @return true when the list is a usable sequence of slots
        bool readImageList(const Json &manifest, OtaBundle &bundleOut, std::string &errorOut)
        {
            const auto found = manifest.find("images");
            if (found == manifest.end() || !found->is_array() || found->empty())
            {
                errorOut = "the manifest lists no image";
                return false;
            }
            int previousSlot = -1;
            for (const Json &entry : *found)
            {
                if (!entry.is_object())
                {
                    errorOut = "the manifest image list holds something that is not an image";
                    return false;
                }
                OtaBundleImage image;
                std::uint32_t slot = 0U;
                if (!readManifestU32(entry, "slot", "image slot", slot, errorOut) ||
                    !readManifestU32(entry, "size", "image size", image.size, errorOut) ||
                    !readManifestU32(entry, "crc32", "image crc32", image.crc32, errorOut))
                {
                    return false;
                }
                if (slot >= OTA_SLOT_COUNT)
                {
                    errorOut = "the manifest describes a slot " + std::to_string(slot) +
                               " this system does not have";
                    return false;
                }
                if (static_cast<int>(slot) <= previousSlot)
                {
                    errorOut = "the manifest images are not in slot order";
                    return false;
                }
                previousSlot = static_cast<int>(slot);
                if (image.size == 0U || image.size > MAX_IMAGE_SIZE)
                {
                    errorOut = "the manifest announces an implausible image size " +
                               std::to_string(image.size) + " bytes";
                    return false;
                }
                image.slot = static_cast<std::uint8_t>(slot);
                bundleOut.images.push_back(std::move(image));
            }
            return true;
        }

        /// @brief Checks the OtaImageHeader an image opens with against the
        ///        manifest entry that describes it. This is the one check
        ///        that catches a bundle assembled from the wrong binaries:
        ///        the header is stamped by the linker and the packaging
        ///        script, the manifest is written by the packaging script,
        ///        and they must tell the same story.
        /// @param bundle bundle being read, for the manifest facts
        /// @param image image to check, bytes included
        /// @param[out] errorOut receives the reason on failure
        /// @return true when header and manifest agree
        bool checkImageHeader(const OtaBundle &bundle,
                              const OtaBundleImage &image,
                              std::string &errorOut)
        {
            const std::string which = "slot " + std::to_string(image.slot);
            if (image.bytes.size() < OTA_IMAGE_HEADER_SIZE)
            {
                errorOut = "the " + which + " image is shorter than its own header";
                return false;
            }
            OtaImageHeader header{};
            std::memcpy(&header, image.bytes.data(), sizeof(header));
            const std::uint32_t magic = header.magic;
            if (magic != OTA_IMAGE_MAGIC)
            {
                errorOut = "the " + which + " image does not start with a firmware header (" +
                           hexText(magic) + ")";
                return false;
            }
            const std::uint16_t headerVersion = header.headerVersion;
            if (headerVersion != OTA_IMAGE_HEADER_VERSION)
            {
                errorOut = "the " + which + " image header is revision " +
                           std::to_string(headerVersion) + ", this hub reads revision " +
                           std::to_string(OTA_IMAGE_HEADER_VERSION);
                return false;
            }
            if (header.mcuId != bundle.mcuId)
            {
                errorOut = "the " + which + " image is built for mcu " +
                           std::to_string(header.mcuId) + " but the manifest says " +
                           std::to_string(bundle.mcuId);
                return false;
            }
            if (header.slotId != image.slot)
            {
                errorOut = "the " + which + " image is linked for slot " +
                           std::to_string(header.slotId) + ", not for the slot it is filed under";
                return false;
            }
            const std::uint32_t headerSize = header.imageSize;
            // An image that was linked but never packaged carries erased
            // bytes here; inside a bundle the packaging script has stamped
            // it, so a placeholder means the bundle was assembled by hand.
            if (headerSize != image.size)
            {
                errorOut = "the " + which + " image header announces " +
                           std::to_string(headerSize) + " bytes, the manifest " +
                           std::to_string(image.size);
                return false;
            }
            return true;
        }
    } // namespace

    const char *otaBundleMagic()
    {
        // Six characters and the two zeros the layout reserves; returned as
        // a pointer because the trailing zeros make it no C string.
        static constexpr char MAGIC[OTA_BUNDLE_MAGIC_SIZE] = {'M', '4', 'O', 'T', 'A', '1', 0, 0};
        return MAGIC;
    }

    std::uint32_t otaImageCrc32(const std::uint8_t *data, std::size_t size)
    {
        std::uint32_t crc = CRC_INIT;
        std::size_t offset = 0U;
        while (offset < size)
        {
            // Bytes are packed into a word in memory order, the tail padded
            // with 0xFF: the F405 hardware unit sees exactly these words
            // when fed from flash, so the two agree bit for bit.
            std::uint32_t word = 0U;
            for (std::size_t byte = 0U; byte < CRC_WORD_BYTES; ++byte)
            {
                const std::uint8_t value =
                    offset + byte < size ? data[offset + byte] : CRC_PAD_BYTE;
                word |= static_cast<std::uint32_t>(value) << (BITS_PER_BYTE * byte);
            }
            offset += CRC_WORD_BYTES;
            crc ^= word;
            for (std::size_t bit = 0U; bit < CRC_WORD_BITS; ++bit)
            {
                const bool top = (crc & CRC_TOP_BIT) != 0U;
                crc <<= 1U;
                if (top)
                {
                    crc ^= CRC_POLYNOMIAL;
                }
            }
        }
        return crc;
    }

    std::string otaVersionText(std::uint8_t major, std::uint8_t minor, std::uint8_t patch)
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    std::string otaGitHashText(const std::array<char, OTA_GIT_HASH_SIZE> &hash)
    {
        std::size_t length = 0U;
        while (length < hash.size() && hash[length] != '\0' &&
               static_cast<unsigned char>(hash[length]) != CRC_PAD_BYTE)
        {
            ++length;
        }
        return {hash.data(), length};
    }

    const OtaBundleImage *findOtaBundleImage(const OtaBundle &bundle, std::uint8_t slot)
    {
        for (const OtaBundleImage &image : bundle.images)
        {
            if (image.slot == slot)
            {
                return &image;
            }
        }
        return nullptr;
    }

    bool loadOtaBundle(const std::string &path, OtaBundle &bundleOut, std::string &errorOut)
    {
        if (path.empty())
        {
            errorOut = "no bundle path given";
            return false;
        }
        std::vector<std::uint8_t> file;
        if (!readWholeFile(path, file))
        {
            errorOut = "cannot read '" + path + "'";
            return false;
        }
        if (file.size() < OTA_BUNDLE_MAGIC_SIZE + LENGTH_FIELD_SIZE)
        {
            errorOut = "'" + path + "' is too short to be a bundle";
            return false;
        }
        if (std::memcmp(file.data(), otaBundleMagic(), OTA_BUNDLE_MAGIC_SIZE) != 0)
        {
            errorOut = "'" + path + "' is not an .ota bundle";
            return false;
        }

        std::size_t cursor = OTA_BUNDLE_MAGIC_SIZE;
        const std::uint32_t manifestSize = readU32(file, cursor);
        cursor += LENGTH_FIELD_SIZE;
        if (manifestSize == 0U || manifestSize > MAX_MANIFEST_SIZE ||
            manifestSize > file.size() - cursor)
        {
            errorOut = "the bundle manifest length is out of bounds";
            return false;
        }
        const Json manifest =
            Json::parse(file.begin() + static_cast<std::ptrdiff_t>(cursor),
                        file.begin() + static_cast<std::ptrdiff_t>(cursor + manifestSize),
                        nullptr,
                        false);
        cursor += manifestSize;
        if (manifest.is_discarded() || !manifest.is_object())
        {
            errorOut = "the bundle manifest is not a JSON object";
            return false;
        }

        OtaBundle bundle;
        bundle.path = path;
        std::uint32_t protocolVersion = 0U;
        if (!readMcuId(manifest, bundle.mcuId, errorOut) ||
            !readVersion(manifest, bundle, errorOut) ||
            !readManifestU32(
                manifest, "protocolVersion", "protocol version", protocolVersion, errorOut) ||
            !readImageList(manifest, bundle, errorOut))
        {
            return false;
        }
        if (protocolVersion != PROTOCOL_VERSION)
        {
            errorOut = "the bundle speaks protocol version " + std::to_string(protocolVersion) +
                       ", this hub speaks " + std::to_string(PROTOCOL_VERSION);
            return false;
        }
        bundle.protocolVersion = static_cast<std::uint8_t>(protocolVersion);
        const auto name = manifest.find("name");
        if (name != manifest.end() && name->is_string())
        {
            bundle.name = name->get<std::string>();
        }
        const auto gitHash = manifest.find("gitHash");
        if (gitHash != manifest.end() && gitHash->is_string())
        {
            bundle.gitHash = gitHash->get<std::string>();
        }

        // The images follow the manifest in the order the manifest lists
        // them, each behind its own length: the manifest size is what the
        // wire announces, the container length is what is actually there,
        // and the two must be the same number.
        for (OtaBundleImage &image : bundle.images)
        {
            const std::string which = "slot " + std::to_string(image.slot);
            if (file.size() - cursor < LENGTH_FIELD_SIZE)
            {
                errorOut = "the bundle ends before its " + which + " image";
                return false;
            }
            const std::uint32_t stored = readU32(file, cursor);
            cursor += LENGTH_FIELD_SIZE;
            if (stored != image.size)
            {
                errorOut = "the bundle holds " + std::to_string(stored) + " bytes for " + which +
                           ", the manifest announces " + std::to_string(image.size);
                return false;
            }
            if (stored > file.size() - cursor)
            {
                errorOut = "the " + which + " image is truncated";
                return false;
            }
            image.bytes.assign(file.begin() + static_cast<std::ptrdiff_t>(cursor),
                               file.begin() + static_cast<std::ptrdiff_t>(cursor + stored));
            cursor += stored;

            if (!checkImageHeader(bundle, image, errorOut))
            {
                return false;
            }
            // The announced CRC is what OTA_BEGIN carries and what the board
            // checks the whole slot against, so it covers the whole image,
            // header included. The post-header value is named too: it is the
            // one other plausible convention, and saying which one matched
            // turns a refusal into a diagnosis.
            const std::uint32_t whole = otaImageCrc32(image.bytes.data(), image.bytes.size());
            if (whole != image.crc32)
            {
                const std::uint32_t afterHeader =
                    otaImageCrc32(image.bytes.data() + OTA_IMAGE_HEADER_SIZE,
                                  image.bytes.size() - OTA_IMAGE_HEADER_SIZE);
                errorOut = "the " + which + " image announces crc32 " + hexText(image.crc32) +
                           " but its bytes hash to " + hexText(whole) + " (" +
                           hexText(afterHeader) + " without the header)";
                return false;
            }
        }
        if (cursor != file.size())
        {
            errorOut = "the bundle carries " + std::to_string(file.size() - cursor) +
                       " trailing bytes that belong to no image";
            return false;
        }

        bundleOut = std::move(bundle);
        return true;
    }
} // namespace mark4
