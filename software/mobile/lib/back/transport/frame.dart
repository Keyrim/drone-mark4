import 'dart:typed_data';

/// Destination meaning "every node".
const int broadcastNode = 0;

/// Bytes of the header in front of every payload.
const int frameHeaderSize = 11;

/// Largest payload one frame carries.
const int maxPayload = 512;

/// Largest frame the link has to carry or accept.
const int maxFrameSize = frameHeaderSize + maxPayload;

/// Relays a frame may cross. The phone relays nothing, so it only ever
/// writes 0 and keeps the ceiling for the record.
const int maxHops = 4;

/// Bits of the last header byte holding the hop count.
const int frameHopsMask = 0x0F;

/// Flag of the last header byte marking the transport's own keepalive.
const int frameFlagKeepalive = 0x80;

/// Payload of a keepalive: the sender's boot id, little-endian u32.
const int keepalivePayloadSize = 4;

/// What every frame opens with: src u32, dst u32, seq u16, flags:hops u8,
/// little endian, 11 bytes. The last byte holds the hop count on its low
/// nibble and the flags on its high one, the keepalive flag being bit 7.
/// The payload behind it is opaque.
class FrameHeader {
  const FrameHeader({
    required this.src,
    required this.dst,
    this.seq = 0,
    this.hops = 0,
    this.keepalive = false,
  });

  /// Reads one header; null when the frame is shorter than one. The other
  /// flag bits are ignored: they are written 0.
  static FrameHeader? decode(Uint8List frame) {
    if (frame.length < frameHeaderSize) {
      return null;
    }
    final data = ByteData.sublistView(frame);
    final flags = data.getUint8(10);
    return FrameHeader(
      src: data.getUint32(0, Endian.little),
      dst: data.getUint32(4, Endian.little),
      seq: data.getUint16(8, Endian.little),
      hops: flags & frameHopsMask,
      keepalive: (flags & frameFlagKeepalive) != 0,
    );
  }

  /// Node that produced the payload.
  final int src;

  /// Node it is for, [broadcastNode] for all.
  final int dst;

  /// Per-sender counter, wraps.
  final int seq;

  /// Relays crossed so far; a sender writes 0.
  final int hops;

  /// The transport's own keepalive, never delivered upward whatever its
  /// payload.
  final bool keepalive;

  /// The header followed by [payload]: one whole frame. The keepalive is
  /// the flagged frame, whose payload is the sender's boot id.
  Uint8List frame(Uint8List payload) {
    final bytes = Uint8List(frameHeaderSize + payload.length);
    final data = ByteData.sublistView(bytes);
    data.setUint32(0, src, Endian.little);
    data.setUint32(4, dst, Endian.little);
    data.setUint16(8, seq, Endian.little);
    data.setUint8(
      10,
      (hops & frameHopsMask) | (keepalive ? frameFlagKeepalive : 0),
    );
    bytes.setRange(frameHeaderSize, bytes.length, payload);
    return bytes;
  }
}
