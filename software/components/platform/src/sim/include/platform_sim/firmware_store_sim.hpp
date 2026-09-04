#pragma once

/// @file
/// @brief File-backed firmware store: the desktop stand-in for the two
///        execute-in-place flash slots and the boot metadata pair of
///        docs/ota-design.md. Four files in one directory - slot_a.bin,
///        slot_b.bin, meta_0.bin, meta_1.bin - which is what lets a desktop
///        flight process run the whole update flow, trial boots included,
///        with no board attached.
///
///        It imitates flash rather than a file: a slot must be erased before
///        it is programmed, programming walks forward and never rewrites
///        bytes it already wrote, bytes nobody ever wrote read back as 0xFF,
///        and the running slot refuses both erase and program. Those
///        refusals are the reason the updater can be trusted on the board:
///        the desktop tests hit the same wall the flash driver is.

#include <array>
#include <cstddef>
#include <cstdint>

#include "ota/firmware_store.hpp"

namespace mark4
{
    /// Two firmware slots and the metadata pair as files in a directory. The
    /// directory content survives the process, so a sim board keeps its
    /// slots and its boot metadata across runs exactly like a real one.
    class FirmwareStoreSim final : public AbsFirmwareStore
    {
      public:
        /// Slot size when the caller does not pick one: the 256 KB order of
        /// magnitude of the real slots, big enough that a firmware image fits
        /// several times over.
        static constexpr std::uint32_t DEFAULT_SLOT_SIZE = 262144U;

        /// Bytes per metadata area. The board erases one 16 KB sector per
        /// area; a file pays only for what it holds, and 4 KB is already 256
        /// records, far more than a session ever appends.
        static constexpr std::uint32_t META_AREA_SIZE = 4096U;

        /// Metadata areas, addressed 0 and 1: the ping-pong pair.
        static constexpr std::size_t META_AREA_COUNT = 2U;

        /// Room for one backing-file path, directory included. A path that
        /// does not fit fails construction rather than being silently cut.
        static constexpr std::size_t MAX_PATH_SIZE = 256U;

        /// @param directory directory holding the four backing files; the
        ///        string is not copied, so it must outlive the store
        /// @param runningSlot slot this process pretends to execute from
        /// @param slotSize bytes available in each slot
        FirmwareStoreSim(const char *directory,
                         std::uint8_t runningSlot,
                         std::uint32_t slotSize = DEFAULT_SLOT_SIZE);

        FirmwareStoreSim(const FirmwareStoreSim &) = delete;
        FirmwareStoreSim &operator=(const FirmwareStoreSim &) = delete;
        FirmwareStoreSim(FirmwareStoreSim &&) = delete;
        FirmwareStoreSim &operator=(FirmwareStoreSim &&) = delete;
        ~FirmwareStoreSim() override = default;

        /// @brief Creates the directory and whatever backing file is missing,
        ///        blank (0xFF) at its full size. Existing files are left
        ///        alone: that is the point of a store that survives the
        ///        process. The reason is logged on failure.
        /// @return true when the four files are ready to be used
        bool init();

        /// @return slot this process executes from
        [[nodiscard]] std::uint8_t runningSlot() const override
        {
            return m_runningSlot;
        }

        /// @return bytes available in each slot
        [[nodiscard]] std::uint32_t slotSize() const override
        {
            return m_slotSize;
        }

        /// @return OTA_MCU_SIM: an image built for a real chip must not be
        ///         accepted here, and the other way around
        [[nodiscard]] std::uint8_t mcuId() const override
        {
            return OTA_MCU_SIM;
        }

        /// @brief Fills one whole slot file with 0xFF and reopens it to
        ///        programming from offset 0.
        /// @param slot slot to erase, never the running one
        /// @return false on a file error or a refused slot
        bool eraseSlot(std::uint8_t slot) override;

        /// @brief Programs bytes into an erased slot, at an offset at or
        ///        after everything already programmed.
        /// @param slot slot to program, never the running one
        /// @param offset byte offset from the slot base
        /// @param data bytes to program
        /// @param size byte count
        /// @return false on a file error, a refused slot, a slot that was
        ///         not erased, or an offset that would rewrite programmed
        ///         bytes
        bool program(std::uint8_t slot,
                     std::uint32_t offset,
                     const std::uint8_t *data,
                     std::uint32_t size) override;

        /// @brief Reads slot bytes back; anything never written reads 0xFF.
        /// @param slot slot to read
        /// @param offset byte offset from the slot base
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        /// @return false on a file error or a range outside the slot
        bool read(std::uint8_t slot,
                  std::uint32_t offset,
                  std::uint8_t *dataOut,
                  std::uint32_t size) const override;

        /// @brief CRC-32/MPEG-2 of a slot range, the tail padded with 0xFF
        ///        (see protocol/ota_image.hpp). Bytes outside the slot, or in a
        ///        file shorter than the range, count as 0xFF.
        /// @param slot slot to checksum
        /// @param offset byte offset from the slot base
        /// @param size byte count before padding
        /// @return the checksum, or the untouched CRC seed for an unknown
        ///         slot
        [[nodiscard]] std::uint32_t crc32(std::uint8_t slot,
                                          std::uint32_t offset,
                                          std::uint32_t size) const override;

        /// @brief Reads the newest valid metadata record from the pair of
        ///        area files.
        /// @param[out] stateOut record content, or its defaults when the log
        ///             is blank
        /// @return false only on a file error
        bool readMeta(OtaMetaState &stateOut) const override;

        /// @brief Appends one metadata record to the area pair.
        /// @param state logical content to persist
        /// @return false on a file error
        bool writeMeta(const OtaMetaState &state) override;

        /// @brief The slot file opens with an OtaImageHeader for the sim
        ///        chip and this slot (ota/image_header.hpp).
        /// @param slot slot to check
        /// @param imageSize bytes the transfer announced
        /// @return false when the slot must not be staged
        [[nodiscard]] bool imageValid(std::uint8_t slot, std::uint32_t imageSize) const override;

        /// @brief Build identity out of the slot's OtaImageHeader.
        /// @param slot slot to read
        /// @param[out] identityOut the identity, valid only on true
        /// @return false when the slot holds no header
        [[nodiscard]] bool readIdentity(std::uint8_t slot,
                                        OtaImageIdentity &identityOut) const override;

        /// @param slot slot index
        /// @return the backing file path of a slot, empty for an unknown one
        [[nodiscard]] const char *slotPath(std::uint8_t slot) const;

        /// @param area metadata area index
        /// @return the backing file path of a metadata area, empty for an
        ///         unknown one
        [[nodiscard]] const char *metaPath(std::size_t area) const;

      private:
        /// One backing-file path, built once at construction.
        using Path = std::array<char, MAX_PATH_SIZE>;

        /// @brief Fills one path buffer with "<directory>/<name>".
        /// @param[out] pathOut buffer to fill
        /// @param name file name inside the directory
        /// @return false when the result would not fit
        bool buildPath(Path &pathOut, const char *name) const;

        /// @param slot slot index
        /// @return true when the slot exists and is not the running one
        [[nodiscard]] bool programmable(std::uint8_t slot) const;

        const char *m_directory;                         ///< run directory, not owned
        std::uint8_t m_runningSlot;                      ///< slot this process runs from
        std::uint32_t m_slotSize;                        ///< bytes per slot
        std::array<Path, OTA_SLOT_COUNT> m_slotPath{};   ///< slot file paths
        std::array<Path, META_AREA_COUNT> m_metaPath{};  ///< metadata area file paths
        std::array<bool, OTA_SLOT_COUNT> m_slotErased{}; ///< erased and open to programming
        std::array<std::uint32_t, OTA_SLOT_COUNT> m_programEnd{}; ///< first byte not programmed
        bool m_pathsOk = false;                                   ///< every path fit its buffer
    };
} // namespace mark4
