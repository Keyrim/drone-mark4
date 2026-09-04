#include "firmware_store_esp32.hpp"

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "log/module.hpp"
#include "log/module_ids.hpp"
#include "ota/crc32_mpeg2.hpp"
#include "protocol/ota_image.hpp"

namespace mark4
{
    namespace
    {
        LogModule MODULE{LOG_MODULE_OTA_STORE, "ota/store"};

        /// Character between the build epoch and the git hash in the version
        /// string the build stamps into the application description.
        constexpr char VERSION_SEPARATOR = '-';

        /// Ticks the task sleeps between two erase blocks: one, the shortest
        /// yield that lets the idle task run.
        constexpr TickType_t ERASE_YIELD_TICKS = 1U;

        /// @param subtype an application partition subtype
        /// @return OTA_SLOT_A for ota_0, OTA_SLOT_B for ota_1, OTA_SLOT_COUNT
        ///         for anything else (a factory partition, a data partition)
        std::uint8_t slotOfSubtype(esp_partition_subtype_t subtype)
        {
            if (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
            {
                return OTA_SLOT_A;
            }
            if (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
            {
                return OTA_SLOT_B;
            }
            return OTA_SLOT_COUNT;
        }

        /// @param partition an application partition
        /// @return true when IDF finds an application description in it: an
        ///         erased slot has none, a slot holding an application has
        bool holdsApplication(const esp_partition_t *partition)
        {
            esp_app_desc_t description = {};
            return esp_ota_get_partition_description(partition, &description) == ESP_OK;
        }

        /// @brief Maps one partition's IDF state to the slot state the
        ///        updater reasons about.
        /// @param partition the slot's partition
        /// @param running true for the slot this image executes from
        /// @return OTA_SLOT_*
        std::uint8_t slotStateOf(const esp_partition_t *partition, bool running)
        {
            esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
            if (esp_ota_get_state_partition(partition, &state) != ESP_OK)
            {
                state = ESP_OTA_IMG_UNDEFINED;
            }
            switch (state)
            {
                case ESP_OTA_IMG_NEW:
                    // Set as the boot partition, not booted yet: the one-shot
                    // trial is still to come.
                    return OTA_SLOT_STAGED;
                case ESP_OTA_IMG_PENDING_VERIFY:
                    return OTA_SLOT_TESTING;
                case ESP_OTA_IMG_VALID:
                    return OTA_SLOT_VALID;
                case ESP_OTA_IMG_INVALID:
                case ESP_OTA_IMG_ABORTED:
                    return OTA_SLOT_BAD;
                case ESP_OTA_IMG_UNDEFINED:
                default:
                    // IDF has no opinion: an image flashed over USB, or the
                    // slot an update was rolled back to. IDF boots such an
                    // image without limits, so it is VALID when there is one,
                    // and the slot is EMPTY when there is none.
                    return (running || holdsApplication(partition)) ? OTA_SLOT_VALID
                                                                    : OTA_SLOT_EMPTY;
            }
        }

        /// @brief Parses "<buildEpoch>-<gitHash>" back out of the version
        ///        string the build stamped.
        /// @param version the esp_app_desc_t version field, terminated
        /// @param[out] identityOut the identity, valid only on true
        /// @return false when the string is not of that shape
        bool parseVersion(const char *version, OtaImageIdentity &identityOut)
        {
            char *end = nullptr;
            const unsigned long epoch = std::strtoul(version, &end, 10);
            if (end == version || *end != VERSION_SEPARATOR || epoch >= OTA_IMAGE_UNSTAMPED)
            {
                return false;
            }
            const char *hash = end + 1;
            const std::size_t length = std::strlen(hash);
            if (length == 0U || length > OTA_GIT_HASH_SIZE)
            {
                return false;
            }
            identityOut.buildEpoch = static_cast<std::uint32_t>(epoch);
            identityOut.gitHash.fill('\0');
            std::memcpy(identityOut.gitHash.data(), hash, length);
            return true;
        }
    } // namespace

    bool FirmwareStoreEsp32::init()
    {
        m_slots[OTA_SLOT_A] = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
        m_slots[OTA_SLOT_B] = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
        if (m_slots[OTA_SLOT_A] == nullptr || m_slots[OTA_SLOT_B] == nullptr)
        {
            MODULE.error("the partition table has no two ota slots");
            return false;
        }
        if (m_slots[OTA_SLOT_A]->size != m_slots[OTA_SLOT_B]->size)
        {
            MODULE.error("the two ota slots differ in size");
            return false;
        }

        const esp_partition_t *running = esp_ota_get_running_partition();
        m_runningSlot = slotOf(running);
        if (m_runningSlot == OTA_SLOT_COUNT)
        {
            MODULE.error("running from a partition that is no ota slot");
            return false;
        }
        m_writeSlot = OTA_SLOT_COUNT;
        m_writeCursor = 0U;

        OtaMetaState meta;
        static_cast<void>(readMeta(meta));
        MODULE.info("running slot %c, states %u/%u, trial %s",
                    m_runningSlot == OTA_SLOT_A ? 'A' : 'B',
                    static_cast<unsigned>(meta.slotState[OTA_SLOT_A]),
                    static_cast<unsigned>(meta.slotState[OTA_SLOT_B]),
                    meta.trialAttempted ? "pending" : "none");
        return true;
    }

    const esp_partition_t *FirmwareStoreEsp32::partitionOf(std::uint8_t slot) const
    {
        return slot < OTA_SLOT_COUNT ? m_slots[slot] : nullptr;
    }

    std::uint8_t FirmwareStoreEsp32::slotOf(const esp_partition_t *partition) const
    {
        if (partition == nullptr)
        {
            return OTA_SLOT_COUNT;
        }
        const std::uint8_t slot = slotOfSubtype(partition->subtype);
        return (slot < OTA_SLOT_COUNT && m_slots[slot] != nullptr) ? slot : OTA_SLOT_COUNT;
    }

    bool FirmwareStoreEsp32::writable(std::uint8_t slot) const
    {
        return partitionOf(slot) != nullptr && slot != m_runningSlot;
    }

    std::uint8_t FirmwareStoreEsp32::runningSlot() const
    {
        return m_runningSlot;
    }

    std::uint32_t FirmwareStoreEsp32::slotSize() const
    {
        const esp_partition_t *partition = partitionOf(OTA_SLOT_A);
        return partition != nullptr ? partition->size : 0U;
    }

    std::uint8_t FirmwareStoreEsp32::mcuId() const
    {
        return OTA_MCU_ESP32C3;
    }

    bool FirmwareStoreEsp32::eraseSlot(std::uint8_t slot)
    {
        if (!writable(slot))
        {
            return false;
        }
        const esp_partition_t *partition = partitionOf(slot);
        for (std::uint32_t offset = 0U; offset < partition->size; offset += ERASE_BLOCK_SIZE)
        {
            const std::uint32_t remaining = partition->size - offset;
            const std::uint32_t block = remaining < ERASE_BLOCK_SIZE ? remaining : ERASE_BLOCK_SIZE;
            const esp_err_t status = esp_partition_erase_range(partition, offset, block);
            if (status != ESP_OK)
            {
                MODULE.error("erase of slot %c failed at %" PRIu32 ": %s",
                             slot == OTA_SLOT_A ? 'A' : 'B',
                             offset,
                             esp_err_to_name(status));
                return false;
            }
            vTaskDelay(ERASE_YIELD_TICKS);
        }
        m_writeSlot = slot;
        m_writeCursor = 0U;
        return true;
    }

    bool FirmwareStoreEsp32::program(std::uint8_t slot,
                                     std::uint32_t offset,
                                     const std::uint8_t *data,
                                     std::uint32_t size)
    {
        if (!writable(slot) || data == nullptr)
        {
            return false;
        }
        const esp_partition_t *partition = partitionOf(slot);
        if (offset > partition->size || (partition->size - offset) < size)
        {
            return false;
        }
        if (slot != m_writeSlot || offset != m_writeCursor)
        {
            return false; // out of order, or a slot not erased since boot
        }
        if (size == 0U)
        {
            return true;
        }
        const esp_err_t status = esp_partition_write(partition, offset, data, size);
        if (status != ESP_OK)
        {
            MODULE.error("write to slot %c failed at %" PRIu32 ": %s",
                         slot == OTA_SLOT_A ? 'A' : 'B',
                         offset,
                         esp_err_to_name(status));
            return false;
        }
        m_writeCursor = offset + size;
        return true;
    }

    bool FirmwareStoreEsp32::read(std::uint8_t slot,
                                  std::uint32_t offset,
                                  std::uint8_t *dataOut,
                                  std::uint32_t size) const
    {
        const esp_partition_t *partition = partitionOf(slot);
        if (partition == nullptr || dataOut == nullptr)
        {
            return false;
        }
        if (offset > partition->size || (partition->size - offset) < size)
        {
            return false;
        }
        return esp_partition_read(partition, offset, dataOut, size) == ESP_OK;
    }

    std::uint32_t FirmwareStoreEsp32::crc32(std::uint8_t slot,
                                            std::uint32_t offset,
                                            std::uint32_t size) const
    {
        Crc32Mpeg2 crc;
        std::uint8_t block[CRC_BLOCK_SIZE];
        std::uint32_t done = 0U;
        while (done < size)
        {
            const std::uint32_t remaining = size - done;
            const std::uint32_t chunk = remaining < CRC_BLOCK_SIZE ? remaining : CRC_BLOCK_SIZE;
            if (!read(slot, offset + done, block, chunk))
            {
                return 0U;
            }
            crc.update(block, chunk);
            done += chunk;
        }
        return crc.finish();
    }

    bool FirmwareStoreEsp32::readMeta(OtaMetaState &stateOut) const
    {
        if (partitionOf(OTA_SLOT_A) == nullptr || partitionOf(OTA_SLOT_B) == nullptr)
        {
            return false;
        }
        // The boot partition is IDF's notion of the active slot. Without an
        // otadata entry it falls back to the first application partition,
        // which is a slot too; anything else means a table this store did
        // not expect, and the running slot is the one fact left.
        const std::uint8_t boot = slotOf(esp_ota_get_boot_partition());
        stateOut.activeSlot = boot < OTA_SLOT_COUNT ? boot : m_runningSlot;
        for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            stateOut.slotState[slot] = slotStateOf(partitionOf(slot), slot == m_runningSlot);
        }
        stateOut.trialAttempted = stateOut.slotState[m_runningSlot] == OTA_SLOT_TESTING;
        return true;
    }

    bool FirmwareStoreEsp32::writeMeta(const OtaMetaState &state)
    {
        OtaMetaState current;
        if (!readMeta(current))
        {
            return false;
        }
        const std::uint8_t other = static_cast<std::uint8_t>(1U - m_runningSlot);

        // The running slot vouched for itself: the trial is over, the IDF
        // bootloader must not roll it back on the next reset.
        if (current.slotState[m_runningSlot] == OTA_SLOT_TESTING &&
            state.slotState[m_runningSlot] == OTA_SLOT_VALID)
        {
            const esp_err_t status = esp_ota_mark_app_valid_cancel_rollback();
            if (status != ESP_OK)
            {
                MODULE.error("cannot confirm the running image: %s", esp_err_to_name(status));
                return false;
            }
        }

        // The other slot was just verified and staged: it is the next boot,
        // and with rollback enabled IDF boots it exactly once as
        // pending-verify. IDF verifies the image again on the way.
        if (current.slotState[other] != OTA_SLOT_STAGED &&
            state.slotState[other] == OTA_SLOT_STAGED)
        {
            const esp_err_t status = esp_ota_set_boot_partition(partitionOf(other));
            if (status != ESP_OK)
            {
                MODULE.error("cannot stage slot %c: %s",
                             other == OTA_SLOT_A ? 'A' : 'B',
                             esp_err_to_name(status));
                return false;
            }
            return true;
        }

        // The other slot is about to be erased while still being the boot
        // partition (a staged image nobody rebooted into): the boot goes
        // back to the image that runs, so a reset during the transfer boots
        // something whole.
        if (state.slotState[other] == OTA_SLOT_EMPTY && current.activeSlot == other &&
            state.activeSlot == other)
        {
            const esp_err_t status = esp_ota_set_boot_partition(partitionOf(m_runningSlot));
            if (status != ESP_OK)
            {
                MODULE.error("cannot hand the boot back: %s", esp_err_to_name(status));
                return false;
            }
            return true;
        }

        // The active slot moved: a revert onto the other, valid image.
        if (state.activeSlot != current.activeSlot && state.activeSlot < OTA_SLOT_COUNT)
        {
            const esp_err_t status = esp_ota_set_boot_partition(partitionOf(state.activeSlot));
            if (status != ESP_OK)
            {
                MODULE.error("cannot activate slot %c: %s",
                             state.activeSlot == OTA_SLOT_A ? 'A' : 'B',
                             esp_err_to_name(status));
                return false;
            }
        }
        return true;
    }

    bool FirmwareStoreEsp32::imageValid(std::uint8_t slot, std::uint32_t imageSize) const
    {
        const esp_partition_t *partition = partitionOf(slot);
        if (partition == nullptr || imageSize > partition->size)
        {
            return false;
        }
        // IDF's own verification: header, segments, checksum and the
        // appended SHA-256, exactly what its bootloader will check. The
        // length it walked must be the length the transfer announced, or the
        // slot holds an image other than the one sent.
        const esp_partition_pos_t position = {partition->address, partition->size};
        esp_image_metadata_t metadata = {};
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK)
        {
            MODULE.warn("slot %c holds no valid application image", slot == OTA_SLOT_A ? 'A' : 'B');
            return false;
        }
        if (metadata.image_len != imageSize)
        {
            MODULE.warn("slot %c image is %" PRIu32 " bytes, the transfer announced %" PRIu32,
                        slot == OTA_SLOT_A ? 'A' : 'B',
                        metadata.image_len,
                        imageSize);
            return false;
        }
        return true;
    }

    bool FirmwareStoreEsp32::readIdentity(std::uint8_t slot, OtaImageIdentity &identityOut) const
    {
        const esp_partition_t *partition = partitionOf(slot);
        if (partition == nullptr)
        {
            return false;
        }
        esp_app_desc_t description = {};
        if (esp_ota_get_partition_description(partition, &description) != ESP_OK)
        {
            return false;
        }
        // The version field is 32 bytes and IDF terminates what it stamps;
        // a copy makes sure the parse stops whatever the field holds.
        char version[sizeof(description.version) + 1U];
        std::memcpy(version, description.version, sizeof(description.version));
        version[sizeof(description.version)] = '\0';
        if (!parseVersion(version, identityOut))
        {
            identityOut.buildEpoch = OTA_IMAGE_UNSTAMPED;
            identityOut.gitHash.fill('\0');
        }
        return true;
    }
} // namespace mark4
