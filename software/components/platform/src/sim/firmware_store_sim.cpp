#include "platform_sim/firmware_store_sim.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "platform_common/crc32_mpeg2.hpp"
#include "platform_common/ota_meta_log.hpp"

namespace mark4
{
    namespace
    {
        /// Erased-flash byte: what an unwritten range reads back as.
        constexpr std::uint8_t BLANK_BYTE = 0xFFU;

        /// Bytes moved per file operation when filling or checksumming. One
        /// buffer on the stack, so neither the erase of a 256 KB slot nor its
        /// checksum allocates anything.
        constexpr std::size_t IO_CHUNK_SIZE = 512U;

        /// Directory permissions of the run directory, the usual 0755.
        constexpr mode_t DIRECTORY_MODE = 0755;

        /// @brief Reports a failed file operation and the reason behind it.
        /// @param what name of the operation that failed
        /// @param path file the operation was applied to
        void logErrno(const char *what, const char *path)
        {
            static_cast<void>(std::fprintf(stderr,
                                           "FirmwareStoreSim: %s failed on '%s': %s\n",
                                           what,
                                           path,
                                           std::strerror(errno)));
        }

        /// @param path file to look for
        /// @return true when the file can be opened for reading
        bool fileExists(const char *path)
        {
            std::FILE *file = std::fopen(path, "rb");
            if (file == nullptr)
            {
                return false;
            }
            static_cast<void>(std::fclose(file));
            return true;
        }

        /// @brief Creates or truncates a file and fills it with 0xFF, which
        ///        is what erasing means on the flash this stands in for.
        /// @param path file to fill
        /// @param size bytes to write
        /// @return false on a file error
        bool fillBlank(const char *path, std::uint32_t size)
        {
            std::FILE *file = std::fopen(path, "wb");
            if (file == nullptr)
            {
                logErrno("fopen", path);
                return false;
            }

            std::array<std::uint8_t, IO_CHUNK_SIZE> blank{};
            blank.fill(BLANK_BYTE);
            bool ok = true;
            std::uint32_t written = 0U;
            while (written < size)
            {
                const std::uint32_t step =
                    ((size - written) < IO_CHUNK_SIZE) ? (size - written) : IO_CHUNK_SIZE;
                if (std::fwrite(blank.data(), 1U, step, file) != step)
                {
                    logErrno("fwrite", path);
                    ok = false;
                    break;
                }
                written += step;
            }
            if (std::fclose(file) != 0)
            {
                logErrno("fclose", path);
                ok = false;
            }
            return ok;
        }

        /// @brief Reads a range, treating everything the file does not hold
        ///        (a missing file, a range past its end) as erased bytes.
        /// @param path file to read
        /// @param offset byte offset in the file
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        /// @return false only on a real file error
        bool readRange(const char *path,
                       std::uint32_t offset,
                       std::uint8_t *dataOut,
                       std::uint32_t size)
        {
            std::memset(dataOut, BLANK_BYTE, size);
            std::FILE *file = std::fopen(path, "rb");
            if (file == nullptr)
            {
                // Nothing was ever written here: erased flash, not an error.
                return true;
            }

            bool ok = true;
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
            {
                logErrno("fseek", path);
                ok = false;
            }
            else
            {
                // A short read is the tail of the range sitting past the end
                // of the file, and the buffer already reads 0xFF there.
                const std::size_t got = std::fread(dataOut, 1U, size, file);
                if (got < size && std::ferror(file) != 0)
                {
                    logErrno("fread", path);
                    ok = false;
                }
            }
            if (std::fclose(file) != 0)
            {
                logErrno("fclose", path);
                ok = false;
            }
            return ok;
        }

        /// @brief Overwrites a range of an existing file.
        /// @param path file to write
        /// @param offset byte offset in the file
        /// @param data bytes to write
        /// @param size byte count
        /// @return false on a file error
        bool writeRange(const char *path,
                        std::uint32_t offset,
                        const std::uint8_t *data,
                        std::uint32_t size)
        {
            std::FILE *file = std::fopen(path, "r+b");
            if (file == nullptr)
            {
                logErrno("fopen", path);
                return false;
            }

            bool ok = true;
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
            {
                logErrno("fseek", path);
                ok = false;
            }
            else if (std::fwrite(data, 1U, size, file) != size)
            {
                logErrno("fwrite", path);
                ok = false;
            }
            if (std::fclose(file) != 0)
            {
                logErrno("fclose", path);
                ok = false;
            }
            return ok;
        }

        /// The two metadata areas as two files: the backend OtaMetaLog runs
        /// on (see platform_common/ota_meta_log.hpp). It holds paths and no
        /// state of its own, so the store builds one per metadata access
        /// instead of keeping a mutable member around.
        class MetaAreaFiles
        {
          public:
            static constexpr std::uint32_t AREA_SIZE = FirmwareStoreSim::META_AREA_SIZE;

            /// @param area0 backing file of area 0
            /// @param area1 backing file of area 1
            MetaAreaFiles(const char *area0, const char *area1)
                : m_paths{area0, area1}
            {
            }

            /// @brief Reads a range of one area; a missing file reads blank.
            /// @param area area index
            /// @param offset byte offset in the area
            /// @param[out] dataOut destination buffer
            /// @param size byte count
            /// @return false on a file error or an out-of-bounds range
            bool read(std::uint8_t area,
                      std::uint32_t offset,
                      std::uint8_t *dataOut,
                      std::uint32_t size)
            {
                if (!InBounds(area, offset, size))
                {
                    return false;
                }
                return readRange(m_paths.at(area), offset, dataOut, size);
            }

            /// @brief Programs a blank range, like the flash underneath: a
            ///        byte that is not 0xFF refuses the write, which is what
            ///        keeps a torn record from being overwritten in place.
            /// @param area area index
            /// @param offset byte offset in the area
            /// @param data bytes to program
            /// @param size byte count
            /// @return false on a file error, an out-of-bounds range or a
            ///         target that is not blank
            bool program(std::uint8_t area,
                         std::uint32_t offset,
                         const std::uint8_t *data,
                         std::uint32_t size)
            {
                if (!InBounds(area, offset, size) || size > IO_CHUNK_SIZE)
                {
                    return false;
                }
                std::array<std::uint8_t, IO_CHUNK_SIZE> current{};
                if (!readRange(m_paths.at(area), offset, current.data(), size))
                {
                    return false;
                }
                for (std::uint32_t i = 0U; i < size; ++i)
                {
                    if (current.at(i) != BLANK_BYTE)
                    {
                        return false;
                    }
                }
                return writeRange(m_paths.at(area), offset, data, size);
            }

            /// @brief Erases one whole area back to 0xFF, creating its file
            ///        when it does not exist yet.
            /// @param area area index
            /// @return false on a file error or an unknown area
            bool erase(std::uint8_t area)
            {
                if (area >= FirmwareStoreSim::META_AREA_COUNT)
                {
                    return false;
                }
                return fillBlank(m_paths.at(area), AREA_SIZE);
            }

          private:
            /// @param area area index
            /// @param offset byte offset in the area
            /// @param size byte count
            /// @return true when the range sits inside an existing area
            static bool InBounds(std::uint8_t area, std::uint32_t offset, std::uint32_t size)
            {
                return area < FirmwareStoreSim::META_AREA_COUNT && offset <= AREA_SIZE &&
                       (AREA_SIZE - offset) >= size;
            }

            std::array<const char *, FirmwareStoreSim::META_AREA_COUNT> m_paths; ///< area files
        };
    } // namespace

    FirmwareStoreSim::FirmwareStoreSim(const char *directory,
                                       std::uint8_t runningSlot,
                                       std::uint32_t slotSize)
        : m_directory(directory),
          m_runningSlot((runningSlot == OTA_SLOT_B) ? OTA_SLOT_B : OTA_SLOT_A),
          m_slotSize(slotSize)
    {
        m_pathsOk = buildPath(m_slotPath.at(OTA_SLOT_A), "slot_a.bin") &&
                    buildPath(m_slotPath.at(OTA_SLOT_B), "slot_b.bin") &&
                    buildPath(m_metaPath.at(0U), "meta_0.bin") &&
                    buildPath(m_metaPath.at(1U), "meta_1.bin");
    }

    bool FirmwareStoreSim::init()
    {
        if (!m_pathsOk)
        {
            static_cast<void>(
                std::fprintf(stderr, "FirmwareStoreSim: backing file paths do not fit\n"));
            return false;
        }

        if (::mkdir(m_directory, DIRECTORY_MODE) != 0 && errno != EEXIST)
        {
            logErrno("mkdir", m_directory);
            return false;
        }

        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            const char *path = m_slotPath.at(slot).data();
            if (fileExists(path))
            {
                continue;
            }
            if (!fillBlank(path, m_slotSize))
            {
                return false;
            }
            // A file created here is blank end to end, which is exactly what
            // an erased slot is: programming it needs no further erase.
            m_slotErased.at(slot) = true;
            m_programEnd.at(slot) = 0U;
        }

        for (std::size_t area = 0U; area < META_AREA_COUNT; ++area)
        {
            const char *path = m_metaPath.at(area).data();
            if (!fileExists(path) && !fillBlank(path, META_AREA_SIZE))
            {
                return false;
            }
        }
        return true;
    }

    bool FirmwareStoreSim::eraseSlot(std::uint8_t slot)
    {
        if (!m_pathsOk || !programmable(slot))
        {
            return false;
        }
        if (!fillBlank(m_slotPath.at(slot).data(), m_slotSize))
        {
            return false;
        }
        m_slotErased.at(slot) = true;
        m_programEnd.at(slot) = 0U;
        return true;
    }

    bool FirmwareStoreSim::program(std::uint8_t slot,
                                   std::uint32_t offset,
                                   const std::uint8_t *data,
                                   std::uint32_t size)
    {
        if (!m_pathsOk || !programmable(slot) || data == nullptr)
        {
            return false;
        }
        if (offset > m_slotSize || (m_slotSize - offset) < size)
        {
            return false;
        }
        if (size == 0U)
        {
            return true;
        }
        // Flash cannot take a byte twice: only an erase reopens a slot, and
        // programming only ever moves forward inside it.
        if (!m_slotErased.at(slot) || offset < m_programEnd.at(slot))
        {
            return false;
        }
        if (!writeRange(m_slotPath.at(slot).data(), offset, data, size))
        {
            return false;
        }
        m_programEnd.at(slot) = offset + size;
        return true;
    }

    bool FirmwareStoreSim::read(std::uint8_t slot,
                                std::uint32_t offset,
                                std::uint8_t *dataOut,
                                std::uint32_t size) const
    {
        if (!m_pathsOk || slot >= OTA_SLOT_COUNT || dataOut == nullptr)
        {
            return false;
        }
        if (offset > m_slotSize || (m_slotSize - offset) < size)
        {
            return false;
        }
        return readRange(m_slotPath.at(slot).data(), offset, dataOut, size);
    }

    std::uint32_t FirmwareStoreSim::crc32(std::uint8_t slot,
                                          std::uint32_t offset,
                                          std::uint32_t size) const
    {
        Crc32Mpeg2 crc;
        if (!m_pathsOk || slot >= OTA_SLOT_COUNT)
        {
            return crc.finish();
        }

        std::array<std::uint8_t, IO_CHUNK_SIZE> buffer{};
        std::uint32_t done = 0U;
        while (done < size)
        {
            const std::uint32_t step =
                ((size - done) < IO_CHUNK_SIZE) ? (size - done) : IO_CHUNK_SIZE;
            // Straight to the file: a range reaching past the slot or past
            // what was written counts as erased bytes, like the flash it
            // stands in for.
            if (!readRange(m_slotPath.at(slot).data(), offset + done, buffer.data(), step))
            {
                break;
            }
            crc.update(buffer.data(), step);
            done += step;
        }
        return crc.finish();
    }

    bool FirmwareStoreSim::readMeta(OtaMetaState &stateOut) const
    {
        if (!m_pathsOk)
        {
            return false;
        }
        MetaAreaFiles areas(m_metaPath.at(0U).data(), m_metaPath.at(1U).data());
        OtaMetaLog<MetaAreaFiles> log(areas);
        return log.read(stateOut);
    }

    bool FirmwareStoreSim::writeMeta(const OtaMetaState &state)
    {
        if (!m_pathsOk)
        {
            return false;
        }
        MetaAreaFiles areas(m_metaPath.at(0U).data(), m_metaPath.at(1U).data());
        OtaMetaLog<MetaAreaFiles> log(areas);
        return log.append(state);
    }

    const char *FirmwareStoreSim::slotPath(std::uint8_t slot) const
    {
        return (slot < OTA_SLOT_COUNT) ? m_slotPath.at(slot).data() : "";
    }

    const char *FirmwareStoreSim::metaPath(std::size_t area) const
    {
        return (area < META_AREA_COUNT) ? m_metaPath.at(area).data() : "";
    }

    bool FirmwareStoreSim::buildPath(Path &pathOut, const char *name) const
    {
        if (m_directory == nullptr)
        {
            return false;
        }
        const int written =
            std::snprintf(pathOut.data(), pathOut.size(), "%s/%s", m_directory, name);
        return written > 0 && static_cast<std::size_t>(written) < pathOut.size();
    }

    bool FirmwareStoreSim::programmable(std::uint8_t slot) const
    {
        // The running slot is the one guarantee of the whole design: the
        // updater must never touch it, and the store makes sure it cannot.
        return slot < OTA_SLOT_COUNT && slot != m_runningSlot;
    }
} // namespace mark4
