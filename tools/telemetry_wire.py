"""Single Python source of the wire protocol: constants and codecs.

This module is pure Python and has no GUI dependency, so it can be imported
and tested headless. It mirrors software/components/protocol/include/protocol/ (version.hpp,
header.hpp, ports.hpp and the packet headers), which are the source of
truth; every Python tool imports the constants from here and nowhere else.
The golden packet fixtures in CI catch any drift against the C++ layout.

All packets are packed, little-endian, and open with a version byte then a
type byte: nothing is ever demultiplexed by size alone. Stream packets
(telemetry, sim raw) follow with a source id byte and a u16 sequence number.
"""

import math
import struct
from dataclasses import dataclass
from typing import Optional, Tuple

#: First byte of every packet, must match mark4::PROTOCOL_VERSION.
PROTOCOL_VERSION = 15

# Packet types, the second byte of every packet (mark4::PacketType).
TYPE_SIM_SENSOR = 1
TYPE_SIM_ACTUATOR = 2
TYPE_TELEMETRY = 3
TYPE_SIM_RAW = 4
TYPE_SIM_SCENARIO = 5
TYPE_RC_COMMAND = 6
TYPE_REBOOT_COMMAND = 7
TYPE_BLACKBOX_RECORD = 8
TYPE_ANNOUNCE = 9
TYPE_TUNING_SET = 10
TYPE_TUNING_GET = 11
TYPE_TUNING_LIST = 12
TYPE_TUNING_ACK = 13
TYPE_TUNING_INFO = 14
TYPE_SIM_RUN_STATS = 15
TYPE_OTA_STATUS_REQUEST = 16
TYPE_OTA_STATUS = 17
TYPE_OTA_BEGIN = 18
TYPE_OTA_CHUNK = 19
TYPE_OTA_CHUNK_ACK = 20
TYPE_OTA_FINISH = 21
TYPE_OTA_CONFIRM = 22
TYPE_OTA_REVERT = 23
TYPE_OTA_ABORT = 24
TYPE_OTA_ACK = 25

# Stream source identities (mark4::StreamSource).
SOURCE_FIRMWARE = 1
SOURCE_DRONE_SIM = 2
SOURCE_DRONE_REPLAY = 3
SOURCE_SIM_PLANT = 4

# Scenarios carried by SimScenario (commands.hpp). Each one opens with a
# reset; everything after that reset tick is scheduled by the plant.
SIM_SCENARIO_RESET = 1
# Retired since v11: RC travels as RcCommandPacket to the flight process
# command receiver. The value stays reserved so the neighbors keep theirs.
SIM_SCENARIO_RC = 2
SIM_SCENARIO_THROW = 3
SIM_SCENARIO_HAND_THROW = 4

# Flags of SimRunStatsPacket (sim_stats.hpp).
SIM_RUN_FLAG_LOCKSTEP_DEGRADED = 0x01
SIM_RUN_FLAG_HASH_SEALED = 0x02

# Piloting modes carried next to the RC state (commands.hpp). Reserved:
# consumed by the mode feature, defined so the wire never breaks again.
RC_MODE_MANUAL = 0
RC_MODE_ALTITUDE_AUTO = 1

# Third byte of RebootCommandPacket (commands.hpp).
BOARD_REBOOT_MAGIC = 0xB7

# Default UDP ports (ports.hpp).
SIM_LINK_PORT = 47800
TELEMETRY_PORT = 47801
SIM_RAW_PORT = 47802
# 47803 is unassigned: it used to mirror the telemetry broadcast for the
# one consumer whose socket stack could not share a bound port.
# 47804 is unassigned: the simulator binds no listening port any more, and
# scenarios reach it inside the lockstep reply.
RC_COMMAND_PORT = 47805
ANNOUNCE_PORT = 47806

# Wire format, little-endian and packed, mirroring mark4::TelemetryPacket:
#   uint8   version          = PROTOCOL_VERSION
#   uint8   type             = TYPE_TELEMETRY
#   uint8   sourceId         SOURCE_* of the sender
#   uint16  sequence         increments per packet sent, wraps
#   uint64  timestampUs      acquisition time [us]
#   float   gyroRadS[3]      body angular rates [rad/s]
#   float   attitudeQuat[4]  estimated attitude, w x y z
#   float   gyroBiasRadS[3]  estimated gyro bias [rad/s]
#   float   motor[4]         normalized motor commands [0, 1]
#   float   altitudeM        estimated altitude above startup [m]
#   float   verticalVelocityMps  estimated vertical velocity, up [m/s]
#   uint8   throwState       ThrowState of the detector
#   uint32  throwCount       throws detected since startup
#   float   releaseVelocityMps   last release velocity [m/s]
#   uint64  apexTimestampUs  last predicted apex instant [us]
#   float   apexAltitudeM    last predicted apex altitude [m]
#   uint8   flightPhase      FlightPhase of the state machine
#   float   baroAltitudeM    last plausible pressure altitude above the
#                            startup reference [m], the raw channel
TELEMETRY_STRUCT = struct.Struct("<BBBHQ3f4f3f4f2fBIfQfBf")

#: Packed wire size, mirroring mark4::TELEMETRY_PACKET_SIZE.
TELEMETRY_PACKET_SIZE = 103

# Wire format mirroring mark4::SimRawPacket, the exact simulator state:
#   uint8   version, uint8 type = TYPE_SIM_RAW
#   uint8   sourceId = SOURCE_SIM_PLANT, uint16 sequence
#   uint64  timestampUs      simulated time [us]
#   float   attitudeQuat[4]  exact attitude, w x y z
#   float   positionM[3]     world position [m]
#   float   velocityMps[3]   world linear velocity [m/s]
SIM_RAW_STRUCT = struct.Struct("<BBBHQ4f3f3f")

#: Packed wire size, mirroring mark4::SIM_RAW_PACKET_SIZE.
SIM_RAW_PACKET_SIZE = 53

# Wire format mirroring mark4::SimSensorPacket (sim_link.hpp): version,
# type, timestampUs, gyro[3], accel[3], baro, reset, session id, lockstep
# timeouts. Sensors only since v11: the pilot state reaches the flight
# process out-of-band, as an RcCommandPacket on its command receiver.
SIM_SENSOR_STRUCT = struct.Struct("<BBQ3f3ffBIH")
SIM_SENSOR_PACKET_SIZE = 45

# Wire format mirroring mark4::SimActuatorPacket (sim_link.hpp): version,
# type, echoed timestampUs, motor[4], then the scenario block repeated on
# every reply (SIM_SCENARIO_FIELDS below, at SIM_ACTUATOR_SCENARIO_OFFSET).
SIM_ACTUATOR_STRUCT = struct.Struct("<BBQ4fBBQII3f3f4f")
SIM_ACTUATOR_PACKET_SIZE = 84
SIM_ACTUATOR_SCENARIO_OFFSET = 26

# Wire format mirroring mark4::SimScenarioPacket (commands.hpp): version,
# type, then the scenario block: sequence, scenario, seed, throw delay,
# hash window, velocity[3], angular[3], held, held tilt, held azimuth,
# swing.
SIM_SCENARIO_STRUCT = struct.Struct("<BBBBQII3f3f4f")
SIM_SCENARIO_PACKET_SIZE = 60
SIM_SCENARIO_SIZE = 58

# Wire format mirroring mark4::SimRunStatsPacket (sim_stats.hpp): version,
# type, source id, sequence, run id, flags, run start, run hash, duplicate
# frames, lockstep timeouts.
SIM_RUN_STATS_STRUCT = struct.Struct("<BBBHBBQQII")
SIM_RUN_STATS_PACKET_SIZE = 31

# Wire format mirroring mark4::RcCommandPacket (commands.hpp): version,
# type, kill, arm, mode, throttle.
RC_COMMAND_STRUCT = struct.Struct("<BBBBBf")
RC_COMMAND_PACKET_SIZE = 9

# Wire format mirroring mark4::RebootCommandPacket (commands.hpp).
REBOOT_COMMAND_STRUCT = struct.Struct("<BBB")
REBOOT_COMMAND_PACKET_SIZE = 3

# Wire format mirroring mark4::AnnouncePacket (announce.hpp): version,
# type, kind, session id, telemetry port, command port. Reserved: nothing
# emits it yet.
ANNOUNCE_STRUCT = struct.Struct("<BBBIHH")
ANNOUNCE_PACKET_SIZE = 11

# Wire formats mirroring the tuning packets (tuning.hpp).
# Statuses carried by a TuningAckPacket.
TUNING_ACK_OK = 0
TUNING_ACK_UNKNOWN_ID = 1
TUNING_ACK_OUT_OF_BOUNDS = 2
TUNING_ACK_LOCKED_WHILE_ARMED = 3

# TuningInfoPacket flags bit: the parameter may change while armed.
TUNING_FLAG_ARMED_CHANGE = 0x01

TUNING_SET_STRUCT = struct.Struct("<BBHf")
TUNING_SET_PACKET_SIZE = 8
TUNING_GET_STRUCT = struct.Struct("<BBH")
TUNING_GET_PACKET_SIZE = 4
TUNING_LIST_STRUCT = struct.Struct("<BBH")
TUNING_LIST_PACKET_SIZE = 4
TUNING_ACK_STRUCT = struct.Struct("<BBHfB")
TUNING_ACK_PACKET_SIZE = 9
TUNING_INFO_STRUCT = struct.Struct("<BBHHH16sfffB")
TUNING_INFO_PACKET_SIZE = 37

# Firmware update packets (ota.hpp). They travel between the hub and the
# board only: the simulator never sees one. Every packet of a transfer
# session echoes the 32-bit session nonce the sender chose at begin, so a
# straggler from an abandoned session cannot corrupt the next one.

# Firmware slot indices; a board carries exactly two.
OTA_SLOT_A = 0
OTA_SLOT_B = 1
OTA_SLOT_COUNT = 2

# Slot lifecycle states, reported by OtaStatusPacket. EMPTY is the erased
# flash byte on purpose: an erased slot needs no metadata write.
OTA_SLOT_STAGED = 1
OTA_SLOT_TESTING = 2
OTA_SLOT_VALID = 3
OTA_SLOT_BAD = 4
OTA_SLOT_EMPTY = 0xFF

# Result byte of OtaAckPacket.
OTA_RESULT_OK = 0
OTA_RESULT_DENIED_ARMED = 1
OTA_RESULT_DENIED_VOLTAGE = 2
OTA_RESULT_DENIED_BUSY = 3
OTA_RESULT_BAD_SESSION = 4
OTA_RESULT_BAD_STATE = 5
OTA_RESULT_BAD_IMAGE = 6
OTA_RESULT_CRC_MISMATCH = 7
OTA_RESULT_STORE_FAILURE = 8

# Target chip of an image; a board refuses an image built for another one.
OTA_MCU_STM32F405 = 1
OTA_MCU_STM32F722 = 2
OTA_MCU_SIM = 200

#: Data bytes of one OtaChunkPacket, sized to fit the serial framing.
OTA_CHUNK_DATA_SIZE = 240
#: In-order chunks the board acknowledges at a time (flow control).
OTA_CHUNK_ACK_WINDOW = 16
#: Characters of the git hash carried by images and status.
OTA_GIT_HASH_SIZE = 8

OTA_STATUS_REQUEST_STRUCT = struct.Struct("<BB")
OTA_STATUS_REQUEST_PACKET_SIZE = 2
# version, type, mcu id, running slot, active slot, updater busy, then per
# slot (state, build epoch, git hash), then slot size, max chunk data.
OTA_STATUS_STRUCT = struct.Struct("<BBBBBBBI8sBI8sIH")
OTA_STATUS_PACKET_SIZE = 38
# version, type, session, image size, image crc.
OTA_BEGIN_STRUCT = struct.Struct("<BBIII")
OTA_BEGIN_PACKET_SIZE = 14
# version, type, session, offset, length, data[240].
OTA_CHUNK_STRUCT = struct.Struct("<BBIIB240s")
OTA_CHUNK_PACKET_SIZE = 251
# version, type, session, next expected offset.
OTA_CHUNK_ACK_STRUCT = struct.Struct("<BBII")
OTA_CHUNK_ACK_PACKET_SIZE = 10
OTA_FINISH_STRUCT = struct.Struct("<BBI")
OTA_FINISH_PACKET_SIZE = 6
OTA_CONFIRM_STRUCT = struct.Struct("<BB")
OTA_CONFIRM_PACKET_SIZE = 2
OTA_REVERT_STRUCT = struct.Struct("<BB")
OTA_REVERT_PACKET_SIZE = 2
OTA_ABORT_STRUCT = struct.Struct("<BBI")
OTA_ABORT_PACKET_SIZE = 6
# version, type, session, acknowledged type, result.
OTA_ACK_STRUCT = struct.Struct("<BBIBB")
OTA_ACK_PACKET_SIZE = 8

# The on-flash image header (ota.hpp): the first bytes of every firmware
# slot, written by the packaging script, read by the bootloader, the
# updater and the hub. magic, header version, mcu id, slot id, image size,
# image crc, build epoch, git hash, reserved, header crc.
OTA_IMAGE_HEADER_STRUCT = struct.Struct("<IHBBIII8s480sI")
#: Bytes reserved for the header at the base of a slot (vector table after).
OTA_IMAGE_HEADER_SIZE = 512
#: "M4FW" read as a little-endian word, the first bytes of an image.
OTA_IMAGE_MAGIC = 0x5746344D
#: Layout revision of the image header itself.
OTA_IMAGE_HEADER_VERSION = 1
#: Placeholder of an image linked but never packaged (erased-flash bytes).
OTA_IMAGE_UNSTAMPED = 0xFFFFFFFF
#: Offset the header crc sits at, so it covers everything before it.
OTA_IMAGE_HEADER_CRC_OFFSET = 508

for _wire_struct, _wire_size, _name in (
    (TELEMETRY_STRUCT, TELEMETRY_PACKET_SIZE, "telemetry"),
    (SIM_RAW_STRUCT, SIM_RAW_PACKET_SIZE, "sim raw"),
    (SIM_SENSOR_STRUCT, SIM_SENSOR_PACKET_SIZE, "sim sensor"),
    (SIM_ACTUATOR_STRUCT, SIM_ACTUATOR_PACKET_SIZE, "sim actuator"),
    (SIM_SCENARIO_STRUCT, SIM_SCENARIO_PACKET_SIZE, "sim scenario"),
    (SIM_RUN_STATS_STRUCT, SIM_RUN_STATS_PACKET_SIZE, "sim run stats"),
    (RC_COMMAND_STRUCT, RC_COMMAND_PACKET_SIZE, "rc command"),
    (REBOOT_COMMAND_STRUCT, REBOOT_COMMAND_PACKET_SIZE, "reboot command"),
    (ANNOUNCE_STRUCT, ANNOUNCE_PACKET_SIZE, "announce"),
    (TUNING_SET_STRUCT, TUNING_SET_PACKET_SIZE, "tuning set"),
    (TUNING_GET_STRUCT, TUNING_GET_PACKET_SIZE, "tuning get"),
    (TUNING_LIST_STRUCT, TUNING_LIST_PACKET_SIZE, "tuning list"),
    (TUNING_ACK_STRUCT, TUNING_ACK_PACKET_SIZE, "tuning ack"),
    (TUNING_INFO_STRUCT, TUNING_INFO_PACKET_SIZE, "tuning info"),
    (OTA_STATUS_REQUEST_STRUCT, OTA_STATUS_REQUEST_PACKET_SIZE, "ota status request"),
    (OTA_STATUS_STRUCT, OTA_STATUS_PACKET_SIZE, "ota status"),
    (OTA_BEGIN_STRUCT, OTA_BEGIN_PACKET_SIZE, "ota begin"),
    (OTA_CHUNK_STRUCT, OTA_CHUNK_PACKET_SIZE, "ota chunk"),
    (OTA_CHUNK_ACK_STRUCT, OTA_CHUNK_ACK_PACKET_SIZE, "ota chunk ack"),
    (OTA_FINISH_STRUCT, OTA_FINISH_PACKET_SIZE, "ota finish"),
    (OTA_CONFIRM_STRUCT, OTA_CONFIRM_PACKET_SIZE, "ota confirm"),
    (OTA_REVERT_STRUCT, OTA_REVERT_PACKET_SIZE, "ota revert"),
    (OTA_ABORT_STRUCT, OTA_ABORT_PACKET_SIZE, "ota abort"),
    (OTA_ACK_STRUCT, OTA_ACK_PACKET_SIZE, "ota ack"),
    (OTA_IMAGE_HEADER_STRUCT, OTA_IMAGE_HEADER_SIZE, "ota image header"),
):
    assert _wire_struct.size == _wire_size, (
        f"{_name} wire layout out of sync with software/components/protocol/include/protocol/"
    )


# Blackbox record (blackbox.hpp): self-framing, sync marker "M4" then
# version, type, length, the sensor-step payload and a crc16 over
# version..payload (little-endian). A .m4bb file is a plain sequence of
# records; a damaged record costs only itself.
BLACKBOX_SYNC0 = 0x4D
BLACKBOX_SYNC1 = 0x34
BLACKBOX_RECORD_STRUCT = struct.Struct("<BBBBBQ3f3ffBfB4fH")
BLACKBOX_RECORD_SIZE = 65
#: Version byte of a blackbox record, mirroring mark4::BLACKBOX_VERSION.
#: Deliberately NOT PROTOCOL_VERSION: a stored format outlives the session
#: that wrote it, so it moves only when the record layout moves.
BLACKBOX_VERSION = 14
BLACKBOX_RECORD_PAYLOAD_SIZE = 58

# Serial framing (serial_framing.hpp): SYNC0 SYNC1 length payload crc16,
# the CRC covering the length byte and the payload, little-endian on the
# wire.
SERIAL_SYNC0 = 0xA5
SERIAL_SYNC1 = 0x5A
SERIAL_FRAME_OVERHEAD = 5
CRC16_INIT = 0xFFFF


def crc16(crc: int, data: bytes) -> int:
    """Feed bytes into a running CRC-16/CCITT-FALSE, mark4::crc16."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


# The one checksum of the update system, mark4::crc32Mpeg2 (polynomial
# 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final xor): bytes are
# gathered into 32-bit words in memory order, which is what the F405
# hardware CRC unit sees when fed words read from flash, so the packaging
# script here and the board agree bit for bit. The word packing and the
# 0xFF tail padding are part of the wire contract, not an implementation
# detail.
def crc32_mpeg2(data: bytes) -> int:
    """CRC-32/MPEG-2 over little-endian 32-bit words, tail padded with 0xFF."""
    data = data + b"\xff" * (-len(data) % 4)
    crc = 0xFFFFFFFF
    for i in range(0, len(data), 4):
        crc ^= int.from_bytes(data[i:i + 4], "little")
        for _ in range(32):
            crc = ((crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1) & 0xFFFFFFFF
    return crc


def encode_serial_frame(payload: bytes) -> bytes:
    """Wrap one packet into a serial frame, mark4::encodeSerialFrame."""
    assert 0 < len(payload) <= 255
    body = bytes([len(payload)]) + payload
    crc = crc16(CRC16_INIT, body)
    return bytes([SERIAL_SYNC0, SERIAL_SYNC1]) + body + bytes(
        [crc & 0xFF, crc >> 8])


def valid_blackbox_record(record: bytes) -> bool:
    """Framing check of one candidate record, mark4::validBlackboxRecord."""
    if len(record) != BLACKBOX_RECORD_SIZE:
        return False
    if record[0] != BLACKBOX_SYNC0 or record[1] != BLACKBOX_SYNC1:
        return False
    if (record[2] != BLACKBOX_VERSION or record[3] != TYPE_BLACKBOX_RECORD
            or record[4] != BLACKBOX_RECORD_PAYLOAD_SIZE):
        return False
    carried = record[-2] | record[-1] << 8
    return carried == crc16(CRC16_INIT, record[2:-2])


def iter_blackbox_records(data: bytes):
    """Yield the field tuple of every valid record in a .m4bb byte string.

    Resynchronizes on the record sync marker after damaged bytes, so a
    torn write costs only the record it tore.
    """
    offset = 0
    while offset + BLACKBOX_RECORD_SIZE <= len(data):
        chunk = data[offset:offset + BLACKBOX_RECORD_SIZE]
        if valid_blackbox_record(chunk):
            yield BLACKBOX_RECORD_STRUCT.unpack(chunk)
            offset += BLACKBOX_RECORD_SIZE
        else:
            offset += 1


@dataclass(frozen=True)
class TelemetrySample:
    """One decoded telemetry packet."""

    source_id: int
    sequence: int
    timestamp_us: int
    gyro_rad_s: Tuple[float, float, float]
    attitude_quat: Tuple[float, float, float, float]
    gyro_bias_rad_s: Tuple[float, float, float]
    motor: Tuple[float, float, float, float]
    altitude_m: float
    vertical_velocity_mps: float
    throw_state: int
    throw_count: int
    release_velocity_mps: float
    apex_timestamp_us: int
    apex_altitude_m: float
    flight_phase: int
    baro_altitude_m: float

    @property
    def timestamp_s(self) -> float:
        """Acquisition time in seconds."""
        return self.timestamp_us * 1e-6


@dataclass(frozen=True)
class SimRawSample:
    """One decoded raw simulator state packet."""

    source_id: int
    sequence: int
    timestamp_us: int
    attitude_quat: Tuple[float, float, float, float]
    position_m: Tuple[float, float, float]
    velocity_mps: Tuple[float, float, float]

    @property
    def timestamp_s(self) -> float:
        """Simulated time in seconds."""
        return self.timestamp_us * 1e-6


@dataclass(frozen=True)
class TuningAck:
    """One decoded answer to a tuning set or get."""

    param_id: int
    value: float
    status: int


@dataclass(frozen=True)
class SimRunStatsSample:
    """One decoded run stats packet: what a simulated run amounted to."""

    source_id: int
    sequence: int
    run_id: int
    flags: int
    run_start_us: int
    run_hash: int
    duplicate_frames: int
    lockstep_timeouts: int

    @property
    def run_start_s(self) -> float:
        """Simulated time the run started at, in seconds."""
        return self.run_start_us * 1e-6

    @property
    def sealed(self) -> bool:
        """True once the hash window elapsed and run_hash is final."""
        return bool(self.flags & SIM_RUN_FLAG_HASH_SEALED)

    @property
    def degraded(self) -> bool:
        """True when the link lost a tick during the run."""
        return bool(self.flags & SIM_RUN_FLAG_LOCKSTEP_DEGRADED)


@dataclass(frozen=True)
class OtaSlotStatus:
    """One firmware slot: its lifecycle state and the image identity in it."""

    state: int
    build_epoch: int
    git_hash: bytes


@dataclass(frozen=True)
class OtaStatus:
    """One decoded update status: what runs, from where, what each slot holds."""

    mcu_id: int
    running_slot: int
    active_slot: int
    updater_busy: int
    slots: Tuple[OtaSlotStatus, OtaSlotStatus]
    slot_size: int
    max_chunk_data: int


@dataclass(frozen=True)
class OtaChunk:
    """One decoded image chunk, data already trimmed to the length byte."""

    session: int
    offset: int
    data: bytes


@dataclass(frozen=True)
class OtaChunkAck:
    """One cumulative chunk acknowledgement: everything below next_offset landed."""

    session: int
    next_offset: int


@dataclass(frozen=True)
class OtaAck:
    """One answer to a begin, finish, confirm, revert or abort."""

    session: int
    acked_type: int
    result: int

    @property
    def ok(self) -> bool:
        """True when the board accepted the request."""
        return self.result == OTA_RESULT_OK


@dataclass(frozen=True)
class OtaImageHeader:
    """One decoded image header, the first bytes of a firmware slot."""

    magic: int
    header_version: int
    mcu_id: int
    slot_id: int
    image_size: int
    image_crc: int
    build_epoch: int
    git_hash: bytes
    header_crc: int


def has_header(datagram: bytes, packet_type: int) -> bool:
    """True when the datagram opens with the protocol version and type."""
    return (
        len(datagram) >= 2
        and datagram[0] == PROTOCOL_VERSION
        and datagram[1] == packet_type
    )


def decode_telemetry(datagram: bytes) -> Optional[TelemetrySample]:
    """Decode one telemetry datagram.

    Returns the decoded sample, or None if the datagram has the wrong size,
    version or type. Callers are expected to count and drop those.
    """
    if len(datagram) != TELEMETRY_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_TELEMETRY):
        return None

    fields = TELEMETRY_STRUCT.unpack(datagram)
    return TelemetrySample(
        source_id=fields[2],
        sequence=fields[3],
        timestamp_us=fields[4],
        gyro_rad_s=fields[5:8],
        attitude_quat=fields[8:12],
        gyro_bias_rad_s=fields[12:15],
        motor=fields[15:19],
        altitude_m=fields[19],
        vertical_velocity_mps=fields[20],
        throw_state=fields[21],
        throw_count=fields[22],
        release_velocity_mps=fields[23],
        apex_timestamp_us=fields[24],
        apex_altitude_m=fields[25],
        flight_phase=fields[26],
        baro_altitude_m=fields[27],
    )


def decode_sim_raw(datagram: bytes) -> Optional[SimRawSample]:
    """Decode one raw simulator state datagram, None when not one."""
    if len(datagram) != SIM_RAW_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_SIM_RAW):
        return None

    fields = SIM_RAW_STRUCT.unpack(datagram)
    return SimRawSample(
        source_id=fields[2],
        sequence=fields[3],
        timestamp_us=fields[4],
        attitude_quat=fields[5:9],
        position_m=fields[9:12],
        velocity_mps=fields[12:15],
    )


class StreamClock:
    """Maps one stream's timestamps onto the receiver's clock.

    Every source stamps with its own clock and its own zero (the board
    counts from boot, the simulator from launch), so timestamps from two
    streams cannot share a plot axis directly. The receiver's clock is the
    only one every stream has in common: the first sample anchors the
    stream (local time, source time), later samples are placed relative to
    that anchor, so the source keeps pacing the samples and only the zero
    comes from the receiver.

    A timestamp going backward means the source rebooted (or was
    restarted): the stream is re-anchored at the current local time and
    the reboot is counted, so a consumer can mark it.
    """

    def __init__(self) -> None:
        self._anchor_local_s: float = 0.0
        self._anchor_source_us: Optional[int] = None
        self._last_source_us: int = 0
        #: Number of re-anchors caused by a backward timestamp.
        self.reboots: int = 0

    def to_local(self, timestamp_us: int, local_now_s: float) -> float:
        """Place one source timestamp on the receiver's time axis.

        Returns seconds in the same base as local_now_s. Detects and
        absorbs source reboots; check reboots (or rebooted()) around the
        call to mark them.
        """
        if self._anchor_source_us is None:
            self._anchor_local_s = local_now_s
            self._anchor_source_us = timestamp_us
        elif timestamp_us < self._last_source_us:
            self.reboots += 1
            self._anchor_local_s = local_now_s
            self._anchor_source_us = timestamp_us
        self._last_source_us = timestamp_us
        return self._anchor_local_s + (timestamp_us - self._anchor_source_us) * 1e-6


def euler_deg(quat: Tuple[float, float, float, float]) -> Tuple[float, float, float]:
    """Roll, pitch, yaw in degrees from a w-x-y-z body-to-world quaternion."""
    w, x, y, z = quat
    roll = math.atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y))
    pitch = math.asin(max(-1.0, min(1.0, 2.0 * (w * y - z * x))))
    yaw = math.atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z))
    return tuple(math.degrees(angle) for angle in (roll, pitch, yaw))


def error_angle_deg(
    a: Tuple[float, float, float, float], b: Tuple[float, float, float, float]
) -> float:
    """Rotation angle between two unit quaternions, in degrees."""
    dot = abs(sum(ca * cb for ca, cb in zip(a, b)))
    return math.degrees(2.0 * math.acos(min(1.0, dot)))


def encode_telemetry(sample: TelemetrySample, version: int = PROTOCOL_VERSION) -> bytes:
    """Pack a sample back into a datagram. Useful for tests and fake sources."""
    return TELEMETRY_STRUCT.pack(
        version,
        TYPE_TELEMETRY,
        sample.source_id,
        sample.sequence,
        sample.timestamp_us,
        *sample.gyro_rad_s,
        *sample.attitude_quat,
        *sample.gyro_bias_rad_s,
        *sample.motor,
        sample.altitude_m,
        sample.vertical_velocity_mps,
        sample.throw_state,
        sample.throw_count,
        sample.release_velocity_mps,
        sample.apex_timestamp_us,
        sample.apex_altitude_m,
        sample.flight_phase,
        sample.baro_altitude_m,
    )


def encode_sim_raw(sample: SimRawSample, version: int = PROTOCOL_VERSION) -> bytes:
    """Pack a raw state sample back into a datagram."""
    return SIM_RAW_STRUCT.pack(
        version,
        TYPE_SIM_RAW,
        sample.source_id,
        sample.sequence,
        sample.timestamp_us,
        *sample.attitude_quat,
        *sample.position_m,
        *sample.velocity_mps,
    )


def encode_rc_command(
    kill: int = 0,
    arm: int = 0,
    mode: int = RC_MODE_MANUAL,
    throttle: float = 0.0,
) -> bytes:
    """Pack one RcCommandPacket; stream it, never fire it once (fail-safe)."""
    return RC_COMMAND_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_RC_COMMAND, kill, arm, mode, throttle,
    )


def encode_tuning_set(param_id: int, value: float) -> bytes:
    """Pack one TuningSetPacket; answered by a TuningAckPacket."""
    return TUNING_SET_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_TUNING_SET, param_id, value,
    )


def decode_tuning_ack(datagram: bytes) -> Optional[TuningAck]:
    """Decode one tuning acknowledgement datagram, None when not one."""
    if len(datagram) != TUNING_ACK_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_TUNING_ACK):
        return None
    fields = TUNING_ACK_STRUCT.unpack(datagram)
    return TuningAck(param_id=fields[2], value=fields[3], status=fields[4])


def encode_sim_scenario(
    sequence: int,
    scenario: int,
    seed: int = 0,
    throw_delay_us: int = 0,
    hash_window_us: int = 0,
    velocity: Tuple[float, float, float] = (0.0, 0.0, 0.0),
    angular: Tuple[float, float, float] = (0.0, 0.0, 0.0),
    held_s: float = 0.0,
    held_tilt: float = 0.0,
    held_azimuth: float = 0.0,
    swing_s: float = 0.0,
) -> bytes:
    """Pack one SimScenarioPacket; fields unused by the scenario are ignored.

    The sequence byte is what makes a scenario idempotent: the plant plays a
    block once per change of it, so the same packet may be resent freely.
    Zero means "no scenario", so senders count 1..255 and wrap.
    """
    return SIM_SCENARIO_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_SIM_SCENARIO, sequence, scenario, seed,
        throw_delay_us, hash_window_us,
        *velocity, *angular, held_s, held_tilt, held_azimuth, swing_s,
    )


def decode_sim_run_stats(datagram: bytes) -> Optional[SimRunStatsSample]:
    """Decode one run stats datagram, None when not one."""
    if len(datagram) != SIM_RUN_STATS_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_SIM_RUN_STATS):
        return None

    fields = SIM_RUN_STATS_STRUCT.unpack(datagram)
    return SimRunStatsSample(
        source_id=fields[2],
        sequence=fields[3],
        run_id=fields[4],
        flags=fields[5],
        run_start_us=fields[6],
        run_hash=fields[7],
        duplicate_frames=fields[8],
        lockstep_timeouts=fields[9],
    )


def encode_ota_status_request() -> bytes:
    """Pack one OtaStatusRequestPacket; legal at any time, session or not."""
    return OTA_STATUS_REQUEST_STRUCT.pack(PROTOCOL_VERSION, TYPE_OTA_STATUS_REQUEST)


def encode_ota_status(status: OtaStatus) -> bytes:
    """Pack one OtaStatusPacket; the board side of the wire, for fake boards."""
    fields = [PROTOCOL_VERSION, TYPE_OTA_STATUS,
              status.mcu_id, status.running_slot, status.active_slot,
              status.updater_busy]
    for slot in status.slots:
        fields += [slot.state, slot.build_epoch,
                   slot.git_hash.ljust(OTA_GIT_HASH_SIZE, b"\x00")]
    fields += [status.slot_size, status.max_chunk_data]
    return OTA_STATUS_STRUCT.pack(*fields)


def encode_ota_begin(session: int, image_size: int, image_crc: int) -> bytes:
    """Pack one OtaBeginPacket; the board erases before it answers, so the
    acknowledgement deserves a timeout counted in seconds."""
    return OTA_BEGIN_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_OTA_BEGIN, session, image_size, image_crc,
    )


def encode_ota_chunk(session: int, offset: int, data: bytes) -> bytes:
    """Pack one OtaChunkPacket; the padding past the length byte is ignored."""
    assert 0 < len(data) <= OTA_CHUNK_DATA_SIZE
    return OTA_CHUNK_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_OTA_CHUNK, session, offset, len(data), data,
    )


def encode_ota_chunk_ack(session: int, next_offset: int) -> bytes:
    """Pack one OtaChunkAckPacket, the cumulative acknowledgement."""
    return OTA_CHUNK_ACK_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_OTA_CHUNK_ACK, session, next_offset,
    )


def encode_ota_finish(session: int) -> bytes:
    """Pack one OtaFinishPacket; the board CRC-checks the slot and stages it."""
    return OTA_FINISH_STRUCT.pack(PROTOCOL_VERSION, TYPE_OTA_FINISH, session)


def encode_ota_confirm() -> bytes:
    """Pack one OtaConfirmPacket; marks the running trial image valid."""
    return OTA_CONFIRM_STRUCT.pack(PROTOCOL_VERSION, TYPE_OTA_CONFIRM)


def encode_ota_revert() -> bytes:
    """Pack one OtaRevertPacket; the reboot command follows separately."""
    return OTA_REVERT_STRUCT.pack(PROTOCOL_VERSION, TYPE_OTA_REVERT)


def encode_ota_abort(session: int) -> bytes:
    """Pack one OtaAbortPacket; drops the session, back to normal mode."""
    return OTA_ABORT_STRUCT.pack(PROTOCOL_VERSION, TYPE_OTA_ABORT, session)


def encode_ota_ack(session: int, acked_type: int, result: int) -> bytes:
    """Pack one OtaAckPacket; session is 0 for the sessionless requests."""
    return OTA_ACK_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_OTA_ACK, session, acked_type, result,
    )


def decode_ota_status(datagram: bytes) -> Optional[OtaStatus]:
    """Decode one update status datagram, None when not one."""
    if len(datagram) != OTA_STATUS_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_OTA_STATUS):
        return None

    fields = OTA_STATUS_STRUCT.unpack(datagram)
    return OtaStatus(
        mcu_id=fields[2],
        running_slot=fields[3],
        active_slot=fields[4],
        updater_busy=fields[5],
        slots=(OtaSlotStatus(state=fields[6], build_epoch=fields[7], git_hash=fields[8]),
               OtaSlotStatus(state=fields[9], build_epoch=fields[10], git_hash=fields[11])),
        slot_size=fields[12],
        max_chunk_data=fields[13],
    )


def decode_ota_chunk(datagram: bytes) -> Optional[OtaChunk]:
    """Decode one image chunk datagram, None when not one or length is out
    of range. The returned data is trimmed to the length byte: the bytes
    past it are padding and mean nothing."""
    if len(datagram) != OTA_CHUNK_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_OTA_CHUNK):
        return None

    fields = OTA_CHUNK_STRUCT.unpack(datagram)
    length = fields[4]
    if not 0 < length <= OTA_CHUNK_DATA_SIZE:
        return None
    return OtaChunk(session=fields[2], offset=fields[3], data=fields[5][:length])


def decode_ota_chunk_ack(datagram: bytes) -> Optional[OtaChunkAck]:
    """Decode one cumulative chunk acknowledgement, None when not one."""
    if len(datagram) != OTA_CHUNK_ACK_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_OTA_CHUNK_ACK):
        return None
    fields = OTA_CHUNK_ACK_STRUCT.unpack(datagram)
    return OtaChunkAck(session=fields[2], next_offset=fields[3])


def decode_ota_ack(datagram: bytes) -> Optional[OtaAck]:
    """Decode one updater acknowledgement datagram, None when not one."""
    if len(datagram) != OTA_ACK_PACKET_SIZE:
        return None
    if not has_header(datagram, TYPE_OTA_ACK):
        return None
    fields = OTA_ACK_STRUCT.unpack(datagram)
    return OtaAck(session=fields[2], acked_type=fields[3], result=fields[4])


def ota_image_header_crc(header: bytes) -> int:
    """CRC-32/MPEG-2 of everything a header carries before its own crc."""
    return crc32_mpeg2(header[:OTA_IMAGE_HEADER_CRC_OFFSET])


def valid_ota_image_header(header: bytes) -> bool:
    """True when a slot's first bytes are a header this layout understands."""
    if len(header) < OTA_IMAGE_HEADER_SIZE:
        return False
    fields = OTA_IMAGE_HEADER_STRUCT.unpack(header[:OTA_IMAGE_HEADER_SIZE])
    if fields[0] != OTA_IMAGE_MAGIC or fields[1] != OTA_IMAGE_HEADER_VERSION:
        return False
    return fields[9] == ota_image_header_crc(header)


def decode_ota_image_header(header: bytes) -> Optional[OtaImageHeader]:
    """Decode the first bytes of a firmware slot, None when not a header.

    The header crc is checked: an image whose header does not hold together
    is not one this layout may reason about.
    """
    if not valid_ota_image_header(header):
        return None

    fields = OTA_IMAGE_HEADER_STRUCT.unpack(header[:OTA_IMAGE_HEADER_SIZE])
    return OtaImageHeader(
        magic=fields[0],
        header_version=fields[1],
        mcu_id=fields[2],
        slot_id=fields[3],
        image_size=fields[4],
        image_crc=fields[5],
        build_epoch=fields[6],
        git_hash=fields[7],
        header_crc=fields[9],
    )


def encode_ota_image_header(
    mcu_id: int,
    slot_id: int,
    image_size: int,
    image_crc: int,
    build_epoch: int = 0,
    git_hash: bytes = b"",
) -> bytes:
    """Stamp one image header, its own crc computed last.

    What the packaging script writes at the base of a slot: the constant
    fields, then the values only packaging knows (size, image crc, build
    epoch, git hash), then the crc over all of them. Unused room is
    erased-flash 0xFF, so a future field costs no layout change.
    """
    body = OTA_IMAGE_HEADER_STRUCT.pack(
        OTA_IMAGE_MAGIC, OTA_IMAGE_HEADER_VERSION, mcu_id, slot_id,
        image_size, image_crc, build_epoch,
        git_hash.ljust(OTA_GIT_HASH_SIZE, b"\xff"),
        b"\xff" * 480, 0,
    )
    crc = ota_image_header_crc(body)
    return body[:OTA_IMAGE_HEADER_CRC_OFFSET] + struct.pack("<I", crc)
