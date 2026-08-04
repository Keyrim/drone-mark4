"""Decoding of the drone telemetry packets received over UDP.

This module is pure Python and has no GUI dependency, so it can be imported
and tested headless. Keep it in sync with the wire format defined in
protocol/include/protocol/telemetry.hpp (and the version byte in
protocol/include/protocol/version.hpp).
"""

import struct
from dataclasses import dataclass
from typing import Optional, Tuple

# Wire format, little-endian and packed, mirroring mark4::TelemetryPacket:
#   uint8   version      = PROTOCOL_VERSION
#   uint64  timestampUs  acquisition time [us]
#   float   gyroRadS[3]  body angular rates [rad/s]
#   float   motor[4]     normalized motor commands [0, 1]
TELEMETRY_STRUCT = struct.Struct("<BQ3f4f")

#: Packed wire size: version (1) + timestamp (8) + gyro (12) + motors (16).
TELEMETRY_PACKET_SIZE = 37

#: First byte of every packet, must match mark4::PROTOCOL_VERSION.
PROTOCOL_VERSION = 2

#: UDP port telemetry is broadcast to, must match mark4::TELEMETRY_PORT.
TELEMETRY_PORT = 47801

assert TELEMETRY_STRUCT.size == TELEMETRY_PACKET_SIZE, (
    "telemetry wire layout out of sync with protocol/include/protocol/telemetry.hpp"
)


@dataclass(frozen=True)
class TelemetrySample:
    """One decoded telemetry packet."""

    timestamp_us: int
    gyro_rad_s: Tuple[float, float, float]
    motor: Tuple[float, float, float, float]

    @property
    def timestamp_s(self) -> float:
        """Acquisition time in seconds."""
        return self.timestamp_us * 1e-6


def decode_telemetry(datagram: bytes) -> Optional[TelemetrySample]:
    """Decode one datagram.

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
        gyro_rad_s=(fields[2], fields[3], fields[4]),
        motor=(fields[5], fields[6], fields[7], fields[8]),
    )


def encode_telemetry(sample: TelemetrySample, version: int = PROTOCOL_VERSION) -> bytes:
    """Pack a sample back into a datagram. Useful for tests and fake sources."""
    return TELEMETRY_STRUCT.pack(
        version,
        sample.timestamp_us,
        *sample.gyro_rad_s,
        *sample.motor,
    )
