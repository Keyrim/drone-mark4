"""Transport frame codec for the python tools.

The wire itself (every message between the flight processes and the
ground) is software/components/protocol/mark4.proto; the desktop build
generates its python codec into software/build/desktop/gen/python
(mark4_pb2.py, plus mark4_wire_hash.py), and the tools import it from
there. What this module keeps is the one layer under it that protobuf does
not describe: the transport frame header (software/components/transport/)
and its shared discovery port.

Python stdlib only.
"""

import struct
from dataclasses import dataclass
from typing import Optional

# The transport (software/components/transport/): every message between the
# flight processes and the ground tools travels inside a frame, on one
# shared discovery port for broadcasts (transport/udp_link.hpp) and on the
# sender's ephemeral data socket for unicasts.
DISCOVERY_PORT = 47820
#: Destination meaning "every node".
BROADCAST_NODE = 0
#: Relays a frame may cross (mark4::Transport::INITIAL_HOPS).
TRANSPORT_INITIAL_HOPS = 4
# Frame header (transport/frame.hpp): src u32, dst u32, seq u16, hops u8,
# little-endian, then the opaque payload.
TRANSPORT_HEADER_STRUCT = struct.Struct("<IIHB")
TRANSPORT_HEADER_SIZE = 11
#: Largest payload one frame carries (mark4::MAX_PAYLOAD).
TRANSPORT_MAX_PAYLOAD = 512


@dataclass(frozen=True)
class TransportFrame:
    """One decoded transport frame: header fields and the opaque payload."""

    src: int
    dst: int
    seq: int
    hops: int
    payload: bytes


def encode_transport_frame(
    src: int, dst: int, seq: int, payload: bytes, hops: int = TRANSPORT_INITIAL_HOPS
) -> bytes:
    """Put the transport header in front of a payload, mark4::encodeFrameHeader."""
    assert 0 < len(payload) <= TRANSPORT_MAX_PAYLOAD
    return TRANSPORT_HEADER_STRUCT.pack(src, dst, seq & 0xFFFF, hops) + payload


def decode_transport_frame(datagram: bytes) -> Optional[TransportFrame]:
    """Split one datagram into header and payload, None when too short."""
    if len(datagram) <= TRANSPORT_HEADER_SIZE:
        return None
    src, dst, seq, hops = TRANSPORT_HEADER_STRUCT.unpack_from(datagram)
    return TransportFrame(src, dst, seq, hops, datagram[TRANSPORT_HEADER_SIZE:])
