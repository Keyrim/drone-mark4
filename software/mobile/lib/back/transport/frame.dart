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

/// What every frame opens with: src u32, dst u32, seq u16, hops u8, little
/// endian, 11 bytes. The payload behind it is opaque.
class FrameHeader {
  const FrameHeader({
    required this.src,
    required this.dst,
    this.seq = 0,
    this.hops = 0,
  });

  /// Reads one header; null when the frame is shorter than one.
  static FrameHeader? decode(Uint8List frame) {
    if (frame.length < frameHeaderSize) {
      return null;
    }
    final data = ByteData.sublistView(frame);
    return FrameHeader(
      src: data.getUint32(0, Endian.little),
      dst: data.getUint32(4, Endian.little),
      seq: data.getUint16(8, Endian.little),
      hops: data.getUint8(10),
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

  /// The header followed by [payload]: one whole frame. An empty payload
  /// makes the header-only frame, which is the transport's keepalive.
  Uint8List frame(Uint8List payload) {
    final bytes = Uint8List(frameHeaderSize + payload.length);
    final data = ByteData.sublistView(bytes);
    data.setUint32(0, src, Endian.little);
    data.setUint32(4, dst, Endian.little);
    data.setUint16(8, seq, Endian.little);
    data.setUint8(10, hops);
    bytes.setRange(frameHeaderSize, bytes.length, payload);
    return bytes;
  }
}
