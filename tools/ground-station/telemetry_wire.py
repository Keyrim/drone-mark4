"""Single Python source of the wire protocol: constants and codecs.

This module is pure Python and has no GUI dependency, so it can be imported
and tested headless. It mirrors protocol/include/protocol/ (version.hpp,
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
PROTOCOL_VERSION = 10

# Packet types, the second byte of every packet (mark4::PacketType).
TYPE_SIM_SENSOR = 1
TYPE_SIM_ACTUATOR = 2
TYPE_TELEMETRY = 3
TYPE_SIM_RAW = 4
TYPE_SIM_COMMAND = 5
TYPE_RC_COMMAND = 6
TYPE_REBOOT_COMMAND = 7
TYPE_BLACKBOX_RECORD = 8
TYPE_ANNOUNCE = 9
TYPE_TUNING_SET = 10
TYPE_TUNING_GET = 11
TYPE_TUNING_LIST = 12
TYPE_TUNING_ACK = 13
TYPE_TUNING_INFO = 14

# Stream source identities (mark4::StreamSource).
SOURCE_FIRMWARE = 1
SOURCE_DRONE_SIM = 2
SOURCE_DRONE_REPLAY = 3
SOURCE_SIM_PLANT = 4

# Scenario commands carried by SimCommandPacket (commands.hpp).
SIM_COMMAND_RESET = 1
SIM_COMMAND_RC = 2
SIM_COMMAND_THROW = 3
SIM_COMMAND_HAND_THROW = 4

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
TELEMETRY_MIRROR_PORT = 47803
SIM_COMMAND_PORT = 47804
RC_COMMAND_PORT = 47805

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
TELEMETRY_STRUCT = struct.Struct("<BBBHQ3f4f3f4f2fBIfQfB")

#: Packed wire size, mirroring mark4::TELEMETRY_PACKET_SIZE.
TELEMETRY_PACKET_SIZE = 99

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
# type, timestampUs, gyro[3], accel[3], baro, kill, throttle, arm, reset.
SIM_SENSOR_STRUCT = struct.Struct("<BBQ3f3ffBfBB")
SIM_SENSOR_PACKET_SIZE = 45

# Wire format mirroring mark4::SimActuatorPacket (sim_link.hpp): version,
# type, echoed timestampUs, motor[4].
SIM_ACTUATOR_STRUCT = struct.Struct("<BBQ4f")
SIM_ACTUATOR_PACKET_SIZE = 26

# Wire format mirroring mark4::SimCommandPacket (commands.hpp): version,
# type, command, kill, arm, mode, throttle, velocity[3], angular[3],
# held, held tilt, held azimuth, swing.
SIM_COMMAND_STRUCT = struct.Struct("<BBBBBBf3f3f4f")
SIM_COMMAND_PACKET_SIZE = 50

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

# Wire formats mirroring the tuning packets (tuning.hpp). Reserved:
# nothing emits them yet.
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

for _wire_struct, _wire_size, _name in (
    (TELEMETRY_STRUCT, TELEMETRY_PACKET_SIZE, "telemetry"),
    (SIM_RAW_STRUCT, SIM_RAW_PACKET_SIZE, "sim raw"),
    (SIM_SENSOR_STRUCT, SIM_SENSOR_PACKET_SIZE, "sim sensor"),
    (SIM_ACTUATOR_STRUCT, SIM_ACTUATOR_PACKET_SIZE, "sim actuator"),
    (SIM_COMMAND_STRUCT, SIM_COMMAND_PACKET_SIZE, "sim command"),
    (RC_COMMAND_STRUCT, RC_COMMAND_PACKET_SIZE, "rc command"),
    (REBOOT_COMMAND_STRUCT, REBOOT_COMMAND_PACKET_SIZE, "reboot command"),
    (ANNOUNCE_STRUCT, ANNOUNCE_PACKET_SIZE, "announce"),
    (TUNING_SET_STRUCT, TUNING_SET_PACKET_SIZE, "tuning set"),
    (TUNING_GET_STRUCT, TUNING_GET_PACKET_SIZE, "tuning get"),
    (TUNING_LIST_STRUCT, TUNING_LIST_PACKET_SIZE, "tuning list"),
    (TUNING_ACK_STRUCT, TUNING_ACK_PACKET_SIZE, "tuning ack"),
    (TUNING_INFO_STRUCT, TUNING_INFO_PACKET_SIZE, "tuning info"),
):
    assert _wire_struct.size == _wire_size, (
        f"{_name} wire layout out of sync with protocol/include/protocol/"
    )


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


def encode_serial_frame(payload: bytes) -> bytes:
    """Wrap one packet into a serial frame, mark4::encodeSerialFrame."""
    assert 0 < len(payload) <= 255
    body = bytes([len(payload)]) + payload
    crc = crc16(CRC16_INIT, body)
    return bytes([SERIAL_SYNC0, SERIAL_SYNC1]) + body + bytes(
        [crc & 0xFF, crc >> 8])


def telemetry_mirror_port(telemetry_port: int) -> int:
    """Mirror of a telemetry port, mark4::telemetryMirrorPort."""
    return telemetry_port + 2


assert telemetry_mirror_port(TELEMETRY_PORT) == TELEMETRY_MIRROR_PORT


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


def encode_sim_command(
    command: int,
    kill: int = 0,
    arm: int = 0,
    mode: int = RC_MODE_MANUAL,
    throttle: float = 0.0,
    velocity: Tuple[float, float, float] = (0.0, 0.0, 0.0),
    angular: Tuple[float, float, float] = (0.0, 0.0, 0.0),
    held_s: float = 0.0,
    held_tilt: float = 0.0,
    held_azimuth: float = 0.0,
    swing_s: float = 0.0,
) -> bytes:
    """Pack one SimCommandPacket; fields unused by the command are ignored."""
    return SIM_COMMAND_STRUCT.pack(
        PROTOCOL_VERSION, TYPE_SIM_COMMAND, command, kill, arm, mode, throttle,
        *velocity, *angular, held_s, held_tilt, held_azimuth, swing_s,
    )
