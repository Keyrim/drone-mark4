#pragma once

/// @file
/// @brief Common packet header: a version byte then a type byte open every
///        packet, so nothing is ever demultiplexed by size alone.
///
///        Portability contract of every wire struct, for whatever compiler
///        builds the next consumer (the ESP32 bridge above all): packed
///        structs (pragma pack(1), no padding anywhere), little-endian
///        byte order, IEEE-754 binary32 floats, 8-bit bytes. The
///        static_asserts on sizes and field offsets check the first
///        assumption on every toolchain that compiles the headers; the
///        endianness assert below checks the second. Multi-byte fields
///        sit at unaligned offsets by design: access them through memcpy,
///        never by binding a reference to a packed member.

#include <bit>
#include <cstddef>
#include <cstdint>

#include "protocol/version.hpp"

namespace mark4
{
    static_assert(std::endian::native == std::endian::little,
                  "the wire is little-endian and so must be every consumer that memcpys structs");

    /// Packet type, the second byte of every packet.
    enum class PacketType : std::uint8_t
    {
        SIM_SENSOR = 1U,          ///< SimSensorPacket
        SIM_ACTUATOR = 2U,        ///< SimActuatorPacket
        TELEMETRY = 3U,           ///< TelemetryPacket
        SIM_RAW = 4U,             ///< SimRawPacket
        SIM_SCENARIO = 5U,        ///< SimScenarioPacket
        RC_COMMAND = 6U,          ///< RcCommandPacket
        REBOOT_COMMAND = 7U,      ///< RebootCommandPacket
        BLACKBOX_RECORD = 8U,     ///< BlackboxRecord
        ANNOUNCE = 9U,            ///< AnnouncePacket
        TUNING_SET = 10U,         ///< TuningSetPacket
        TUNING_GET = 11U,         ///< TuningGetPacket
        TUNING_LIST = 12U,        ///< TuningListPacket
        TUNING_ACK = 13U,         ///< TuningAckPacket
        TUNING_INFO = 14U,        ///< TuningInfoPacket
        SIM_RUN_STATS = 15U,      ///< SimRunStatsPacket
        OTA_STATUS_REQUEST = 16U, ///< OtaStatusRequestPacket
        OTA_STATUS = 17U,         ///< OtaStatusPacket
        OTA_BEGIN = 18U,          ///< OtaBeginPacket
        OTA_CHUNK = 19U,          ///< OtaChunkPacket
        OTA_CHUNK_ACK = 20U,      ///< OtaChunkAckPacket
        OTA_FINISH = 21U,         ///< OtaFinishPacket
        OTA_CONFIRM = 22U,        ///< OtaConfirmPacket
        OTA_REVERT = 23U,         ///< OtaRevertPacket
        OTA_ABORT = 24U,          ///< OtaAbortPacket
        OTA_ACK = 25U,            ///< OtaAckPacket
    };

    /// Identity of a stream sender, carried by every stream packet
    /// (telemetry, sim raw) so several senders coexist on one port and a
    /// consumer can tell a live sim from a replay or a real board.
    enum class StreamSource : std::uint8_t
    {
        FIRMWARE = 1U,     ///< the real board
        DRONE_SIM = 2U,    ///< desktop flight process against the plant
        DRONE_REPLAY = 3U, ///< re-execution of a recorded flight
        SIM_PLANT = 4U,    ///< the physics simulator itself (sim raw)
    };

    /// @brief Checks the two header bytes of a received packet.
    /// @param data packet bytes
    /// @param size packet size in bytes
    /// @param type expected packet type
    /// @return true when the packet starts with PROTOCOL_VERSION and type
    constexpr bool hasHeader(const std::uint8_t *data, std::size_t size, PacketType type)
    {
        return size >= 2U && data[0] == PROTOCOL_VERSION &&
               data[1] == static_cast<std::uint8_t>(type);
    }
} // namespace mark4
