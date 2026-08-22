#include "platform_stm32/ota_meta_flash.hpp"

namespace mark4
{
    namespace
    {
        /// @brief Checks an area index and a byte range against one area.
        /// @param area area index as given by OtaMetaLog
        /// @param offset byte offset inside the area
        /// @param size byte count
        /// @return true when the whole range sits inside a known area
        bool rangeFits(std::uint8_t area, std::uint32_t offset, std::uint32_t size)
        {
            if (area >= OTA_META_AREA_COUNT)
            {
                return false;
            }
            return offset <= OTA_META_AREA_SIZE && size <= OTA_META_AREA_SIZE - offset;
        }
    } // namespace

    bool OtaMetaFlashBackend::read(std::uint8_t area,
                                   std::uint32_t offset,
                                   std::uint8_t *dataOut,
                                   std::uint32_t size) const
    {
        if (!rangeFits(area, offset, size))
        {
            return false;
        }
        InternalFlash::Read(otaMetaAreaBase(area) + offset, dataOut, size);
        return true;
    }

    bool OtaMetaFlashBackend::program(std::uint8_t area,
                                      std::uint32_t offset,
                                      const std::uint8_t *data,
                                      std::uint32_t size)
    {
        if (!rangeFits(area, offset, size))
        {
            return false;
        }
        return m_flash.program(otaMetaAreaBase(area) + offset, data, size);
    }

    bool OtaMetaFlashBackend::erase(std::uint8_t area)
    {
        if (area >= OTA_META_AREA_COUNT)
        {
            return false;
        }
        return m_flash.eraseSector(otaMetaAreaSector(area));
    }
} // namespace mark4
