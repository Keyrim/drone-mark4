#pragma once

/// @file
/// @brief Blackbox record wire format. The record crosses process
///        boundaries (UART stream today, SD dump later) and is stored in
///        .m4bb files, so it lives in protocol/ and carries its own
///        version byte. Each record carries its own sync marker, type
///        byte, length and CRC: a decoder resynchronizes after a torn
///        write (power loss on impact) at the cost of the damaged record
///        only, skips record types it does not know, and old logs stay
///        readable forever.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"
#include "protocol/serial_framing.hpp"

namespace mark4
{
    /// Sync marker opening every blackbox record: "M4".
    inline constexpr std::uint8_t BLACKBOX_SYNC0 = 0x4DU;
    inline constexpr std::uint8_t BLACKBOX_SYNC1 = 0x34U;

    /// Version byte of the record below, and the only thing a decoder
    /// accepts in that position. It is deliberately NOT PROTOCOL_VERSION:
    /// a stored format outlives the session that wrote it, and the promise
    /// above ("old logs stay readable forever") cannot hold if a change to
    /// an unrelated live packet invalidates every file on disk. It moves
    /// only when the record layout below moves, which is what a reader
    /// actually needs to know.
    inline constexpr std::uint8_t BLACKBOX_VERSION = 14U;

    /// Bytes between the length byte and the CRC of a sensor-step record.
    inline constexpr std::uint8_t BLACKBOX_RECORD_PAYLOAD_SIZE = 58U;

#pragma pack(push, 1)
    /// One recorded step: the sensor frame that entered the core and the
    /// actuator frame it produced. The record is self-framing (sync,
    /// version, type, length, CRC), so a log file is a plain sequence of
    /// records and the same bytes travel unchanged over any transport.
    /// The CRC covers version through payload and travels little-endian.
    struct BlackboxRecord
    {
        std::uint8_t sync0 = 0U;          ///< = BLACKBOX_SYNC0
        std::uint8_t sync1 = 0U;          ///< = BLACKBOX_SYNC1
        std::uint8_t version = 0U;        ///< = BLACKBOX_VERSION
        std::uint8_t type = 0U;           ///< = PacketType::BLACKBOX_RECORD
        std::uint8_t length = 0U;         ///< = BLACKBOX_RECORD_PAYLOAD_SIZE;
                                          ///< lets a decoder skip a record
                                          ///< type it does not know
        std::uint64_t timestampUs = 0U;   ///< acquisition time [us]
        std::array<float, 3> gyroRadS{};  ///< body angular rates [rad/s]
        std::array<float, 3> accelMps2{}; ///< specific force [m/s^2] (0 g in free fall)
        float baroPa = 0.0f;              ///< static pressure [Pa]
        std::uint8_t killSwitch = 0U;     ///< 1 = engaged (motors cut), 0 = released
        float throttle = 0.0f;            ///< normalized RC throttle [0, 1]
        std::uint8_t armSwitch = 0U;      ///< 1 = armed for an autonomous throw flight
        std::array<float, 4> motor{};     ///< normalized motor commands [0, 1]
        std::uint16_t crc = 0U;           ///< crc16 over version..payload
    };
#pragma pack(pop)

    /// Packed record size: sync (2) + version (1) + type (1) + length (1)
    /// + payload (58) + crc (2).
    inline constexpr std::size_t BLACKBOX_RECORD_SIZE = 65U;

    /// Offset of the first CRC-covered byte (the version byte).
    inline constexpr std::size_t BLACKBOX_CRC_FIRST_BYTE = 2U;

    /// Offset of the CRC itself, which ends the coverage.
    inline constexpr std::size_t BLACKBOX_CRC_OFFSET = BLACKBOX_RECORD_SIZE - 2U;

    static_assert(sizeof(BlackboxRecord) == BLACKBOX_RECORD_SIZE, "record layout must be packed");
    // The size alone cannot catch a field reinterpreted in place (same
    // bytes, new meaning), which is exactly the change that must move the
    // version a decoder trusts. Touching either number below is the moment
    // to ask whether old files can still be read.
    // Both literals ARE the named facts here: repeating them is what makes
    // this assert fire when either constant moves alone.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(BLACKBOX_RECORD_SIZE == 65U && BLACKBOX_VERSION == 14U,
                  "the record layout moved: bump BLACKBOX_VERSION and update this assert");
    // NOLINTEND(readability-magic-numbers)
    static_assert(std::is_trivially_copyable_v<BlackboxRecord>);
    static_assert(offsetof(BlackboxRecord, sync0) == 0U,
                  "the sync marker must come first, it is what a decoder hunts for");
    static_assert(offsetof(BlackboxRecord, crc) == BLACKBOX_CRC_OFFSET);
    static_assert(BLACKBOX_CRC_OFFSET - offsetof(BlackboxRecord, timestampUs) ==
                      BLACKBOX_RECORD_PAYLOAD_SIZE,
                  "the length byte must describe the payload exactly");

    /// @brief CRC of a serialized record, over version through payload.
    /// @param record the full record bytes, BLACKBOX_RECORD_SIZE of them
    /// @return the CRC the record should carry
    constexpr std::uint16_t blackboxRecordCrc(const std::uint8_t *record)
    {
        return crc16(CRC16_INIT,
                     record + BLACKBOX_CRC_FIRST_BYTE,
                     BLACKBOX_CRC_OFFSET - BLACKBOX_CRC_FIRST_BYTE);
    }

    /// @brief Checks the framing of a serialized record: sync marker,
    ///        version, type, length and CRC.
    /// @param record candidate record bytes, BLACKBOX_RECORD_SIZE of them
    /// @return true when the bytes are one valid sensor-step record
    inline bool validBlackboxRecord(const std::uint8_t *record)
    {
        if (record[0] != BLACKBOX_SYNC0 || record[1] != BLACKBOX_SYNC1)
        {
            return false;
        }
        if (record[2] != BLACKBOX_VERSION ||
            record[3] != static_cast<std::uint8_t>(PacketType::BLACKBOX_RECORD) ||
            record[4] != BLACKBOX_RECORD_PAYLOAD_SIZE)
        {
            return false;
        }
        const auto carried = static_cast<std::uint16_t>(
            record[BLACKBOX_CRC_OFFSET] |
            static_cast<std::uint16_t>(record[BLACKBOX_CRC_OFFSET + 1U]) << CRC16_BYTE_SHIFT);
        return carried == blackboxRecordCrc(record);
    }
} // namespace mark4
