#pragma once

/// @file
/// @brief Firmware update packets and the on-flash image header, per
///        docs/ota-design.md. The board holds two execute-in-place
///        firmware slots; the hub streams an image into the inactive one
///        in acknowledged chunks, the board stages it after a CRC check,
///        a one-shot trial boot follows and the hub confirms or the
///        bootloader rolls back. Every updater packet answers through one
///        OtaAckPacket except the chunk flow, which has its own
///        cumulative acknowledgement.
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

#include "protocol/header.hpp"

namespace mark4
{
    /// Firmware slot indices; a board carries exactly two.
    inline constexpr std::uint8_t OTA_SLOT_A = 0U;
    inline constexpr std::uint8_t OTA_SLOT_B = 1U;
    inline constexpr std::size_t OTA_SLOT_COUNT = 2U;

    /// Slot lifecycle states, persisted in the boot metadata and reported
    /// by OtaStatusPacket. EMPTY is the erased-flash byte on purpose: a
    /// freshly erased slot needs no metadata write to be in its state.
    inline constexpr std::uint8_t OTA_SLOT_STAGED = 1U;   ///< transfer complete, CRC verified
    inline constexpr std::uint8_t OTA_SLOT_TESTING = 2U;  ///< one-shot trial boot in progress
    inline constexpr std::uint8_t OTA_SLOT_VALID = 3U;    ///< confirmed by the ground side
    inline constexpr std::uint8_t OTA_SLOT_BAD = 4U;      ///< trial failed or CRC mismatch
    inline constexpr std::uint8_t OTA_SLOT_EMPTY = 0xFFU; ///< erased or never written

    /// Result byte of OtaAckPacket.
    inline constexpr std::uint8_t OTA_RESULT_OK = 0U;
    inline constexpr std::uint8_t OTA_RESULT_DENIED_ARMED = 1U;   ///< no update while armed
    inline constexpr std::uint8_t OTA_RESULT_DENIED_VOLTAGE = 2U; ///< battery under the floor
    inline constexpr std::uint8_t OTA_RESULT_DENIED_BUSY = 3U;    ///< another session is open
    inline constexpr std::uint8_t OTA_RESULT_BAD_SESSION = 4U;    ///< unknown session nonce
    inline constexpr std::uint8_t OTA_RESULT_BAD_STATE = 5U;      ///< request illegal right now
    inline constexpr std::uint8_t OTA_RESULT_BAD_IMAGE = 6U; ///< header refused (mcu, slot, size)
    inline constexpr std::uint8_t OTA_RESULT_CRC_MISMATCH = 7U; ///< slot CRC differs from announced
    inline constexpr std::uint8_t OTA_RESULT_STORE_FAILURE =
        8U; ///< erase, program or meta write failed

    /// Target chip of an image; a board refuses an image built for
    /// another one.
    inline constexpr std::uint8_t OTA_MCU_STM32F405 = 1U;
    inline constexpr std::uint8_t OTA_MCU_STM32F722 = 2U;
    inline constexpr std::uint8_t OTA_MCU_SIM = 200U; ///< desktop flight processes

    /// Data bytes of one OtaChunkPacket. Sized so the packet fits the
    /// 255-byte serial framing payload with room to spare.
    inline constexpr std::size_t OTA_CHUNK_DATA_SIZE = 240U;

    /// The board acknowledges every this-many in-order chunks (and any
    /// out-of-order one immediately); the sender keeps at most one window
    /// in flight, which is also the flow control protecting the bridge's
    /// UART transmit side.
    inline constexpr std::uint32_t OTA_CHUNK_ACK_WINDOW = 16U;

    /// Characters of the git hash carried by images and status: ASCII
    /// hex, zero-padded; all 0xFF in an image never packaged (an elf
    /// flashed over SWD during development).
    inline constexpr std::size_t OTA_GIT_HASH_SIZE = 8U;

#pragma pack(push, 1)
    /// Asks for one OtaStatusPacket; legal at any time, session or not.
    struct OtaStatusRequestPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::OTA_STATUS_REQUEST
    };

    /// The board's update-side identity: what runs, from where, and what
    /// each slot holds. The hub picks the image variant to send from
    /// runningSlot and drives auto-confirm from the version fields.
    struct OtaStatusPacket
    {
        std::uint8_t version;     ///< = PROTOCOL_VERSION
        std::uint8_t type;        ///< = PacketType::OTA_STATUS
        std::uint8_t mcuId;       ///< OTA_MCU_* of this board
        std::uint8_t runningSlot; ///< slot this firmware executes from
        std::uint8_t activeSlot;  ///< slot the boot metadata prefers; differs from
                                  ///< runningSlot during a trial boot and after a revert
        std::array<std::uint8_t, OTA_SLOT_COUNT> slotState; ///< OTA_SLOT_* per slot
        std::uint8_t updaterBusy;                           ///< 1 while a transfer session is open
        std::uint8_t versionMajor;                          ///< running firmware version
        std::uint8_t versionMinor;                          ///< running firmware version
        std::uint8_t versionPatch;                          ///< running firmware version
        std::array<char, OTA_GIT_HASH_SIZE> gitHash;        ///< running image git hash
        std::uint32_t slotSize;     ///< bytes available per slot on this chip
        std::uint16_t maxChunkData; ///< largest chunk data size the board accepts
    };

    /// Opens a transfer session into the inactive slot. The board erases
    /// that slot before answering, which takes seconds: the sender allows
    /// the OtaAckPacket a generous timeout.
    struct OtaBeginPacket
    {
        std::uint8_t version;    ///< = PROTOCOL_VERSION
        std::uint8_t type;       ///< = PacketType::OTA_BEGIN
        std::uint32_t session;   ///< nonce chosen by the sender, echoed everywhere
        std::uint32_t imageSize; ///< total image bytes, header included
        std::uint32_t imageCrc;  ///< CRC-32/MPEG-2 of the whole image
    };

    /// One in-order piece of the image. The board writes strictly
    /// sequentially: a chunk whose offset is not the next expected byte
    /// is dropped and answered with the repeated expected offset.
    struct OtaChunkPacket
    {
        std::uint8_t version;  ///< = PROTOCOL_VERSION
        std::uint8_t type;     ///< = PacketType::OTA_CHUNK
        std::uint32_t session; ///< session nonce from OtaBeginPacket
        std::uint32_t offset;  ///< byte offset of this chunk in the image
        std::uint8_t length;   ///< valid bytes in data, 1 to OTA_CHUNK_DATA_SIZE
        std::array<std::uint8_t, OTA_CHUNK_DATA_SIZE> data; ///< image bytes
    };

    /// Cumulative acknowledgement: everything below nextOffset is
    /// written; the sender resumes from there (go-back-N).
    struct OtaChunkAckPacket
    {
        std::uint8_t version;     ///< = PROTOCOL_VERSION
        std::uint8_t type;        ///< = PacketType::OTA_CHUNK_ACK
        std::uint32_t session;    ///< session nonce echoed
        std::uint32_t nextOffset; ///< first image byte still missing
    };

    /// Ends the transfer: the board CRC-checks the full image in flash
    /// against OtaBeginPacket's announcement, validates the image header,
    /// and stages the slot.
    struct OtaFinishPacket
    {
        std::uint8_t version;  ///< = PROTOCOL_VERSION
        std::uint8_t type;     ///< = PacketType::OTA_FINISH
        std::uint32_t session; ///< session nonce echoed
    };

    /// Marks the running trial image VALID and its slot active. Sent by
    /// the hub once the new firmware has proven its link.
    struct OtaConfirmPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::OTA_CONFIRM
    };

    /// Activates the other slot if it holds a VALID image; the reboot
    /// command follows separately.
    struct OtaRevertPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::OTA_REVERT
    };

    /// Drops the open session; the slot stays half-written and EMPTY-like
    /// until the next OtaBeginPacket erases it again.
    struct OtaAbortPacket
    {
        std::uint8_t version;  ///< = PROTOCOL_VERSION
        std::uint8_t type;     ///< = PacketType::OTA_ABORT
        std::uint32_t session; ///< session nonce echoed
    };

    /// The one answer packet of the updater: acknowledges a begin,
    /// finish, confirm, revert or abort with OTA_RESULT_OK or the refusal
    /// reason. session is 0 for the sessionless requests (confirm,
    /// revert, and a begin refused before a session existed).
    struct OtaAckPacket
    {
        std::uint8_t version;   ///< = PROTOCOL_VERSION
        std::uint8_t type;      ///< = PacketType::OTA_ACK
        std::uint32_t session;  ///< session nonce, or 0 when none applies
        std::uint8_t ackedType; ///< PacketType value being answered
        std::uint8_t result;    ///< OTA_RESULT_*
    };
#pragma pack(pop)

    /// version (1) + type (1).
    inline constexpr std::size_t OTA_STATUS_REQUEST_PACKET_SIZE = 2U;

    /// version (1) + type (1) + mcu (1) + running slot (1) + active slot
    /// (1) + slot states (2) + busy (1) + version (3) + git hash (8) +
    /// slot size (4) + max chunk (2).
    inline constexpr std::size_t OTA_STATUS_PACKET_SIZE = 25U;

    /// version (1) + type (1) + session (4) + image size (4) + crc (4).
    inline constexpr std::size_t OTA_BEGIN_PACKET_SIZE = 14U;

    /// version (1) + type (1) + session (4) + offset (4) + length (1) +
    /// data (240).
    inline constexpr std::size_t OTA_CHUNK_PACKET_SIZE = 251U;

    /// version (1) + type (1) + session (4) + next offset (4).
    inline constexpr std::size_t OTA_CHUNK_ACK_PACKET_SIZE = 10U;

    /// version (1) + type (1) + session (4).
    inline constexpr std::size_t OTA_FINISH_PACKET_SIZE = 6U;

    /// version (1) + type (1).
    inline constexpr std::size_t OTA_CONFIRM_PACKET_SIZE = 2U;

    /// version (1) + type (1).
    inline constexpr std::size_t OTA_REVERT_PACKET_SIZE = 2U;

    /// version (1) + type (1) + session (4).
    inline constexpr std::size_t OTA_ABORT_PACKET_SIZE = 6U;

    /// version (1) + type (1) + session (4) + acked type (1) + result (1).
    inline constexpr std::size_t OTA_ACK_PACKET_SIZE = 8U;

    static_assert(sizeof(OtaStatusRequestPacket) == OTA_STATUS_REQUEST_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(sizeof(OtaStatusPacket) == OTA_STATUS_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaBeginPacket) == OTA_BEGIN_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaChunkPacket) == OTA_CHUNK_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaChunkAckPacket) == OTA_CHUNK_ACK_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(sizeof(OtaFinishPacket) == OTA_FINISH_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaConfirmPacket) == OTA_CONFIRM_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(sizeof(OtaRevertPacket) == OTA_REVERT_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaAbortPacket) == OTA_ABORT_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(OtaAckPacket) == OTA_ACK_PACKET_SIZE, "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<OtaStatusPacket>);
    static_assert(std::is_trivially_copyable_v<OtaChunkPacket>);

    /// The chunk packet must fit the one-byte length field of the serial
    /// framing with margin for its overhead.
    // NOLINTNEXTLINE(readability-magic-numbers): the framing's own length limit
    static_assert(OTA_CHUNK_PACKET_SIZE <= 255U, "chunk must fit the serial framing payload");

    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(OtaStatusPacket, mcuId) == 2U);
    static_assert(offsetof(OtaStatusPacket, runningSlot) == 3U);
    static_assert(offsetof(OtaStatusPacket, activeSlot) == 4U);
    static_assert(offsetof(OtaStatusPacket, slotState) == 5U);
    static_assert(offsetof(OtaStatusPacket, updaterBusy) == 7U);
    static_assert(offsetof(OtaStatusPacket, versionMajor) == 8U);
    static_assert(offsetof(OtaStatusPacket, gitHash) == 11U);
    static_assert(offsetof(OtaStatusPacket, slotSize) == 19U);
    static_assert(offsetof(OtaStatusPacket, maxChunkData) == 23U);
    static_assert(offsetof(OtaBeginPacket, session) == 2U);
    static_assert(offsetof(OtaBeginPacket, imageSize) == 6U);
    static_assert(offsetof(OtaBeginPacket, imageCrc) == 10U);
    static_assert(offsetof(OtaChunkPacket, session) == 2U);
    static_assert(offsetof(OtaChunkPacket, offset) == 6U);
    static_assert(offsetof(OtaChunkPacket, length) == 10U);
    static_assert(offsetof(OtaChunkPacket, data) == 11U);
    static_assert(offsetof(OtaChunkAckPacket, session) == 2U);
    static_assert(offsetof(OtaChunkAckPacket, nextOffset) == 6U);
    static_assert(offsetof(OtaFinishPacket, session) == 2U);
    static_assert(offsetof(OtaAbortPacket, session) == 2U);
    static_assert(offsetof(OtaAckPacket, session) == 2U);
    static_assert(offsetof(OtaAckPacket, ackedType) == 6U);
    static_assert(offsetof(OtaAckPacket, result) == 7U);
    // NOLINTEND(readability-magic-numbers)

    /// First bytes of an image: "M4FW" read as a little-endian word.
    inline constexpr std::uint32_t OTA_IMAGE_MAGIC = 0x5746344DU;

    /// Layout revision of OtaImageHeader itself.
    inline constexpr std::uint16_t OTA_IMAGE_HEADER_VERSION = 1U;

    /// Bytes reserved for the header at the base of every slot; the
    /// vector table follows it, which keeps the table 512-byte aligned.
    inline constexpr std::size_t OTA_IMAGE_HEADER_SIZE = 512U;

    /// The imageCrc / imageSize / gitHash placeholder of an image that
    /// was linked but never packaged (an elf flashed over SWD during
    /// development): erased-flash bytes. The bootloader boots such an
    /// image without a CRC check; the OTA path always stamps real values
    /// (scripts/make_ota.py), so every image that travelled the wire is
    /// verified on every boot.
    inline constexpr std::uint32_t OTA_IMAGE_UNSTAMPED = 0xFFFFFFFFU;

    /// Bytes of the header left unused, which is what makes OtaImageHeader
    /// exactly OTA_IMAGE_HEADER_SIZE long: room for a signature later.
    inline constexpr std::size_t OTA_IMAGE_RESERVED_SIZE = 480U;

#pragma pack(push, 1)
    /// The first OTA_IMAGE_HEADER_SIZE bytes of every firmware slot. The
    /// constant fields (magic through slotId, version numbers) are filled
    /// at compile time by the firmware itself; imageSize, imageCrc and
    /// gitHash are stamped into the binary by the packaging script. It
    /// crosses process boundaries like any wire struct: the packaging
    /// script writes it, the bootloader, the updater and the hub read it.
    struct OtaImageHeader
    {
        std::uint32_t magic;         ///< = OTA_IMAGE_MAGIC
        std::uint16_t headerVersion; ///< = OTA_IMAGE_HEADER_VERSION
        std::uint8_t mcuId;          ///< OTA_MCU_* this image was built for
        std::uint8_t slotId;         ///< slot this image was linked for
        std::uint32_t imageSize;     ///< total image bytes, header included
        std::uint32_t imageCrc;      ///< CRC-32/MPEG-2 of the bytes after the header
        std::uint8_t versionMajor;   ///< firmware version
        std::uint8_t versionMinor;   ///< firmware version
        std::uint8_t versionPatch;   ///< firmware version
        std::uint8_t reserved0;      ///< 0xFF
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
    static_assert(offsetof(OtaImageHeader, versionMajor) == 16U);
    static_assert(offsetof(OtaImageHeader, gitHash) == 20U);
    static_assert(offsetof(OtaImageHeader, headerCrc) == 508U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
