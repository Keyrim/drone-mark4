#pragma once

/// @file
/// @brief The boot metadata log of docs/ota-design.md section 4.4: fixed
///        16-byte records appended into two erase-together areas, newest
///        valid record wins, ping-pong when one fills. Every state change
///        of the update system is one append here, which is what makes
///        power loss at any byte harmless: a torn record fails its CRC
///        and the previous one stands. Shared by the bootloader (flash
///        backend), the stm32 firmware store (same backend) and the sim
///        store (file backend).
///
///        Backend contract (duck-typed template parameter, two areas
///        addressed 0 and 1):
///          static constexpr std::uint32_t AREA_SIZE;   // bytes per area
///          bool read(std::uint8_t area, std::uint32_t offset,
///                    std::uint8_t *dataOut, std::uint32_t size);
///          bool program(std::uint8_t area, std::uint32_t offset,
///                       const std::uint8_t *data, std::uint32_t size);
///          bool erase(std::uint8_t area);              // whole area to 0xFF
///        program() only ever targets record-sized blank (0xFF) ranges.

#include <cstdint>
#include <cstring>

#include "platform/firmware_store.hpp"
#include "platform_common/crc32_mpeg2.hpp"

namespace mark4
{
    template <typename Backend> class OtaMetaLog
    {
      public:
        explicit OtaMetaLog(Backend &backend)
            : m_backend(backend)
        {
        }

        /// @brief Finds the newest valid record across both areas.
        /// @param[out] stateOut filled from that record, or left at its
        ///             defaults when the log is blank
        /// @return false only on backend read failure
        bool read(OtaMetaState &stateOut)
        {
            Scan scan;
            if (!scanAreas(scan))
            {
                return false;
            }
            if (scan.found)
            {
                stateOut.activeSlot = scan.newest.activeSlot;
                stateOut.slotState = scan.newest.slotState;
                stateOut.trialAttempted = (scan.newest.flags & OTA_META_FLAG_TRIAL_ATTEMPTED) != 0U;
            }
            else
            {
                stateOut = OtaMetaState{};
            }
            return true;
        }

        /// @brief Appends one record with the next counter, ping-ponging
        ///        to the other area (erased first) when the current one
        ///        has no blank record slot left.
        /// @param state logical content to persist
        /// @return false on backend failure
        bool append(const OtaMetaState &state)
        {
            Scan scan;
            if (!scanAreas(scan))
            {
                return false;
            }

            std::uint8_t area = scan.found ? scan.newestArea : 0U;
            std::uint32_t offset = 0U;
            if (!firstBlankSlot(area, offset))
            {
                area = static_cast<std::uint8_t>(1U - area);
                if (!m_backend.erase(area))
                {
                    return false;
                }
                offset = 0U;
            }
            else if (!scan.found && offset == 0U)
            {
                // A blank-looking virgin log: erase defensively so a
                // never-initialized backing area is truly 0xFF.
                if (!m_backend.erase(area))
                {
                    return false;
                }
            }

            OtaMetaRecord record{};
            record.counter = scan.found ? scan.newest.counter + 1U : 1U;
            record.activeSlot = state.activeSlot;
            record.slotState = state.slotState;
            record.flags = state.trialAttempted ? OTA_META_FLAG_TRIAL_ATTEMPTED : 0U;
            record.reserved = BLANK_WORD;
            std::uint8_t bytes[OTA_META_RECORD_SIZE];
            std::memcpy(bytes, &record, OTA_META_RECORD_SIZE);
            record.crc = crc32Mpeg2(bytes, offsetof(OtaMetaRecord, crc));
            std::memcpy(bytes, &record, OTA_META_RECORD_SIZE);
            return m_backend.program(area, offset, bytes, OTA_META_RECORD_SIZE);
        }

      private:
        /// Result of one pass over both areas.
        struct Scan
        {
            bool found = false;           ///< at least one valid record exists
            OtaMetaRecord newest{};       ///< highest-counter valid record
            std::uint8_t newestArea = 0U; ///< area holding it
        };

        static constexpr std::uint32_t SLOT_COUNT_PER_AREA =
            Backend::AREA_SIZE / OTA_META_RECORD_SIZE;

        /// The byte an erased cell reads as, hence what a never-written
        /// record slot is full of and what the reserved field is set to.
        static constexpr std::uint8_t BLANK_BYTE = 0xFFU;
        static constexpr std::uint32_t BLANK_WORD = 0xFFFFFFFFU;

        bool scanAreas(Scan &scanOut)
        {
            for (std::uint8_t area = 0U; area < 2U; ++area)
            {
                for (std::uint32_t slot = 0U; slot < SLOT_COUNT_PER_AREA; ++slot)
                {
                    std::uint8_t bytes[OTA_META_RECORD_SIZE];
                    if (!m_backend.read(
                            area, slot * OTA_META_RECORD_SIZE, bytes, OTA_META_RECORD_SIZE))
                    {
                        return false;
                    }
                    if (IsBlank(bytes))
                    {
                        continue;
                    }
                    OtaMetaRecord record{};
                    std::memcpy(&record, bytes, OTA_META_RECORD_SIZE);
                    if (record.crc != crc32Mpeg2(bytes, offsetof(OtaMetaRecord, crc)))
                    {
                        continue; // torn or corrupted: the previous record stands
                    }
                    if (!scanOut.found || record.counter > scanOut.newest.counter)
                    {
                        scanOut.found = true;
                        scanOut.newest = record;
                        scanOut.newestArea = area;
                    }
                }
            }
            return true;
        }

        /// First blank record slot of an area, skipping everything
        /// non-blank (a torn record must never be overwritten in place).
        bool firstBlankSlot(std::uint8_t area, std::uint32_t &offsetOut)
        {
            for (std::uint32_t slot = 0U; slot < SLOT_COUNT_PER_AREA; ++slot)
            {
                std::uint8_t bytes[OTA_META_RECORD_SIZE];
                if (!m_backend.read(area, slot * OTA_META_RECORD_SIZE, bytes, OTA_META_RECORD_SIZE))
                {
                    return false;
                }
                if (IsBlank(bytes))
                {
                    offsetOut = slot * OTA_META_RECORD_SIZE;
                    return true;
                }
            }
            return false;
        }

        static bool IsBlank(const std::uint8_t *bytes)
        {
            for (std::uint32_t i = 0U; i < OTA_META_RECORD_SIZE; ++i)
            {
                if (bytes[i] != BLANK_BYTE)
                {
                    return false;
                }
            }
            return true;
        }

        Backend &m_backend; ///< storage underneath, two erasable areas
    };
} // namespace mark4
