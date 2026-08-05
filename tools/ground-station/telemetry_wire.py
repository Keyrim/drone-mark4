"""Decoding of the drone UDP broadcasts: telemetry and raw simulator state.

This module is pure Python and has no GUI dependency, so it can be imported
and tested headless. Keep it in sync with the wire formats defined in
protocol/include/protocol/telemetry.hpp and protocol/include/protocol/
sim_raw.hpp (and the version byte in protocol/include/protocol/version.hpp).
"""

import math
import struct
from dataclasses import dataclass
from typing import Optional, Tuple

# Wire format, little-endian and packed, mirroring mark4::TelemetryPacket:
#   uint8   version          = PROTOCOL_VERSION
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
TELEMETRY_STRUCT = struct.Struct("<BQ3f4f3f4f2fBIfQf")

#: Packed wire size: version (1) + timestamp (8) + gyro (12) + attitude
#: quaternion (16) + gyro bias (12) + motors (16) + altitude (4) + vz (4)
#: + throw state (1) + throw count (4) + release velocity (4)
#: + apex timestamp (8) + apex altitude (4).
TELEMETRY_PACKET_SIZE = 94

# Wire format mirroring mark4::SimRawPacket, the exact simulator state:
#   uint8   version          = PROTOCOL_VERSION
#   uint64  timestampUs      simulated time [us]
#   float   attitudeQuat[4]  exact attitude, w x y z
#   float   positionM[3]     world position [m]
#   float   velocityMps[3]   world linear velocity [m/s]
SIM_RAW_STRUCT = struct.Struct("<BQ4f3f3f")

#: Packed wire size: version (1) + timestamp (8) + quaternion (16)
#: + position (12) + velocity (12).
SIM_RAW_PACKET_SIZE = 49

#: First byte of every packet, must match mark4::PROTOCOL_VERSION.
PROTOCOL_VERSION = 5

#: UDP port telemetry is broadcast to, must match mark4::TELEMETRY_PORT.
TELEMETRY_PORT = 47801

#: UDP port the simulator broadcasts its raw state to (mark4::SIM_RAW_PORT).
SIM_RAW_PORT = 47802

assert TELEMETRY_STRUCT.size == TELEMETRY_PACKET_SIZE, (
    "telemetry wire layout out of sync with protocol/include/protocol/telemetry.hpp"
)
assert SIM_RAW_STRUCT.size == SIM_RAW_PACKET_SIZE, (
    "sim raw wire layout out of sync with protocol/include/protocol/sim_raw.hpp"
)


@dataclass(frozen=True)
class TelemetrySample:
    """One decoded telemetry packet."""

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

    @property
    def timestamp_s(self) -> float:
        """Acquisition time in seconds."""
        return self.timestamp_us * 1e-6


@dataclass(frozen=True)
class SimRawSample:
    """One decoded raw simulator state packet."""

    timestamp_us: int
    attitude_quat: Tuple[float, float, float, float]
    position_m: Tuple[float, float, float]
    velocity_mps: Tuple[float, float, float]

    @property
    def timestamp_s(self) -> float:
        """Simulated time in seconds."""
        return self.timestamp_us * 1e-6


def decode_telemetry(datagram: bytes) -> Optional[TelemetrySample]:
    """Decode one telemetry datagram.

    Returns the decoded sample, or None if the datagram has the wrong size or
    an unknown protocol version. Callers are expected to count and drop those.
    """
    if len(datagram) != TELEMETRY_PACKET_SIZE:
        return None

    fields = TELEMETRY_STRUCT.unpack(datagram)
    if fields[0] != PROTOCOL_VERSION:
        return None

    return TelemetrySample(
        timestamp_us=fields[1],
        gyro_rad_s=fields[2:5],
        attitude_quat=fields[5:9],
        gyro_bias_rad_s=fields[9:12],
        motor=fields[12:16],
        altitude_m=fields[16],
        vertical_velocity_mps=fields[17],
        throw_state=fields[18],
        throw_count=fields[19],
        release_velocity_mps=fields[20],
        apex_timestamp_us=fields[21],
        apex_altitude_m=fields[22],
    )


def decode_sim_raw(datagram: bytes) -> Optional[SimRawSample]:
    """Decode one raw simulator state datagram, None when not one."""
    if len(datagram) != SIM_RAW_PACKET_SIZE:
        return None

    fields = SIM_RAW_STRUCT.unpack(datagram)
    if fields[0] != PROTOCOL_VERSION:
        return None

    return SimRawSample(
        timestamp_us=fields[1],
        attitude_quat=fields[2:6],
        position_m=fields[6:9],
        velocity_mps=fields[9:12],
    )


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
    )


def encode_sim_raw(sample: SimRawSample, version: int = PROTOCOL_VERSION) -> bytes:
    """Pack a raw state sample back into a datagram."""
    return SIM_RAW_STRUCT.pack(
        version,
        sample.timestamp_us,
        *sample.attitude_quat,
        *sample.position_m,
        *sample.velocity_mps,
    )
