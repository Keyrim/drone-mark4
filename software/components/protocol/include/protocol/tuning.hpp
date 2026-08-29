#pragma once

/// @file
/// @brief Dynamic tuning packets: set / get / list parameters with
///        acknowledgement. The flight core owns a static parameter table
///        (id, value, bounds, armed-change policy) and the ground side
///        reads and writes it through these packets without reflashing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "protocol/header.hpp"

namespace mark4
{
    /// Result of a set or get request, carried by TuningAckPacket.
    inline constexpr std::uint8_t TUNING_ACK_OK = 0U;
    inline constexpr std::uint8_t TUNING_ACK_UNKNOWN_ID = 1U;
    inline constexpr std::uint8_t TUNING_ACK_OUT_OF_BOUNDS = 2U;
    inline constexpr std::uint8_t TUNING_ACK_LOCKED_WHILE_ARMED = 3U;

    /// TuningInfoPacket flags bit: the parameter may change while armed
    /// (rate gains during bring-up: yes; safety thresholds: no).
    inline constexpr std::uint8_t TUNING_FLAG_ARMED_CHANGE = 0x01U;

    /// Characters of a parameter name on the wire: ASCII, zero-padded, a
    /// full-length name carries no terminator.
    inline constexpr std::size_t TUNING_NAME_SIZE = 16U;

#pragma pack(push, 1)
    /// Writes one parameter value; answered by a TuningAckPacket carrying
    /// the value actually in effect. Changes apply between two steps of
    /// the flight loop, never during one.
    struct TuningSetPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::TUNING_SET
        std::uint16_t id;     ///< parameter id from the table
        float value;          ///< requested value
    };

    /// Reads one parameter value; answered by a TuningAckPacket.
    struct TuningGetPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::TUNING_GET
        std::uint16_t id;     ///< parameter id from the table
    };

    /// Walks the parameter table; answered by one TuningInfoPacket per
    /// parameter starting at the given index, so a slow link can page
    /// through the table.
    struct TuningListPacket
    {
        std::uint8_t version;     ///< = PROTOCOL_VERSION
        std::uint8_t type;        ///< = PacketType::TUNING_LIST
        std::uint16_t startIndex; ///< first table index to describe
    };

    /// Answer to a set or get: the status and the value in effect after
    /// the request (unchanged when the request was refused).
    struct TuningAckPacket
    {
        std::uint8_t version; ///< = PROTOCOL_VERSION
        std::uint8_t type;    ///< = PacketType::TUNING_ACK
        std::uint16_t id;     ///< parameter id echoed from the request
        float value;          ///< value in effect after the request
        std::uint8_t status;  ///< one of the TUNING_ACK_* values
    };

    /// One parameter description, answering a list request. The name
    /// travels with the parameter so the ground side never duplicates the
    /// table.
    struct TuningInfoPacket
    {
        std::uint8_t version;                    ///< = PROTOCOL_VERSION
        std::uint8_t type;                       ///< = PacketType::TUNING_INFO
        std::uint16_t index;                     ///< table index of this entry
        std::uint16_t count;                     ///< total table size
        std::uint16_t id;                        ///< parameter id
        std::array<char, TUNING_NAME_SIZE> name; ///< ASCII name, zero-padded
        float value;                             ///< current value
        float minValue;                          ///< lower bound, inclusive
        float maxValue;                          ///< upper bound, inclusive
        std::uint8_t flags;                      ///< TUNING_FLAG_* bits
    };
#pragma pack(pop)

    /// version (1) + type (1) + id (2) + value (4).
    inline constexpr std::size_t TUNING_SET_PACKET_SIZE = 8U;

    /// version (1) + type (1) + id (2).
    inline constexpr std::size_t TUNING_GET_PACKET_SIZE = 4U;

    /// version (1) + type (1) + start index (2).
    inline constexpr std::size_t TUNING_LIST_PACKET_SIZE = 4U;

    /// version (1) + type (1) + id (2) + value (4) + status (1).
    inline constexpr std::size_t TUNING_ACK_PACKET_SIZE = 9U;

    /// version (1) + type (1) + index (2) + count (2) + id (2) + name (16)
    /// + value (4) + min (4) + max (4) + flags (1).
    inline constexpr std::size_t TUNING_INFO_PACKET_SIZE = 37U;

    static_assert(sizeof(TuningSetPacket) == TUNING_SET_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(TuningGetPacket) == TUNING_GET_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(TuningListPacket) == TUNING_LIST_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(sizeof(TuningAckPacket) == TUNING_ACK_PACKET_SIZE, "wire layout must be packed");
    static_assert(sizeof(TuningInfoPacket) == TUNING_INFO_PACKET_SIZE,
                  "wire layout must be packed");
    static_assert(std::is_trivially_copyable_v<TuningSetPacket>);
    static_assert(std::is_trivially_copyable_v<TuningGetPacket>);
    static_assert(std::is_trivially_copyable_v<TuningListPacket>);
    static_assert(std::is_trivially_copyable_v<TuningAckPacket>);
    static_assert(std::is_trivially_copyable_v<TuningInfoPacket>);
    // The offsets ARE the named facts here: each assert freezes one
    // field position of the cross-language wire contract.
    // NOLINTBEGIN(readability-magic-numbers)
    static_assert(offsetof(TuningSetPacket, id) == 2U);
    static_assert(offsetof(TuningSetPacket, value) == 4U);
    static_assert(offsetof(TuningGetPacket, id) == 2U);
    static_assert(offsetof(TuningListPacket, startIndex) == 2U);
    static_assert(offsetof(TuningAckPacket, id) == 2U);
    static_assert(offsetof(TuningAckPacket, value) == 4U);
    static_assert(offsetof(TuningAckPacket, status) == 8U);
    static_assert(offsetof(TuningInfoPacket, index) == 2U);
    static_assert(offsetof(TuningInfoPacket, count) == 4U);
    static_assert(offsetof(TuningInfoPacket, id) == 6U);
    static_assert(offsetof(TuningInfoPacket, name) == 8U);
    static_assert(offsetof(TuningInfoPacket, value) == 24U);
    static_assert(offsetof(TuningInfoPacket, minValue) == 28U);
    static_assert(offsetof(TuningInfoPacket, maxValue) == 32U);
    static_assert(offsetof(TuningInfoPacket, flags) == 36U);
    // NOLINTEND(readability-magic-numbers)
} // namespace mark4
