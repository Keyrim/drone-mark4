import 'dart:typed_data';

import 'package:equatable/equatable.dart';

/// One live node of the transport's table, as the C++ transport keeps it.
class NodeInfo extends Equatable {
  const NodeInfo({
    required this.id,
    required this.host,
    required this.port,
    required this.lastSeenUs,
    required this.received,
    required this.lost,
    required this.duplicates,
  });

  /// Node id, never 0.
  final int id;

  /// IPv4 address, host byte order.
  final int host;

  /// UDP port.
  final int port;

  /// Instant of the last frame from it [us].
  final int lastSeenUs;

  /// Frames accepted from it.
  final int received;

  /// Frames the numbering says never arrived.
  final int lost;

  /// Frames carrying an already seen number.
  final int duplicates;

  /// Dotted IPv4 and port, `192.168.4.1:47821`.
  String get address =>
      '${(host >> 24) & 0xFF}.${(host >> 16) & 0xFF}.${(host >> 8) & 0xFF}.${host & 0xFF}:$port';

  @override
  List<Object?> get props => [
    id,
    host,
    port,
    lastSeenUs,
    received,
    lost,
    duplicates,
  ];
}

/// Counters of the node, transport and shim together.
class TransportStats extends Equatable {
  const TransportStats({
    this.sent = 0,
    this.sentBytes = 0,
    this.refused = 0,
    this.dropped = 0,
    this.rxOverflow = 0,
    this.dataPort = 0,
    this.loopbackFallback = false,
  });

  /// Frames handed to the link, beacons included.
  final int sent;

  /// Payload bytes of those frames.
  final int sentBytes;

  /// Sends that reached no link.
  final int refused;

  /// Frames the transport dropped.
  final int dropped;

  /// Payloads the receive queue had to drop.
  final int rxOverflow;

  /// Port of the unicast socket.
  final int dataPort;

  /// A broadcast had no route and used the loopback.
  final bool loopbackFallback;

  @override
  List<Object?> get props => [
    sent,
    sentBytes,
    refused,
    dropped,
    rxOverflow,
    dataPort,
    loopbackFallback,
  ];
}

/// One payload addressed to this node, as the transport delivered it.
class InboundPayload {
  const InboundPayload({required this.src, required this.bytes});

  /// Sender.
  final int src;

  /// Payload, one encoded Envelope.
  final Uint8List bytes;
}

/// One transport node with one UDP link: the C ABI shim behind dart:ffi, or a
/// fake in tests. Everything is non-blocking; poll() is where frames flow.
abstract class AbsTransportNode {
  /// Identity of this node, never 0.
  int get nodeId;

  /// Registers the beacon broadcast every second and unicast to every node
  /// the moment it appears. False when the payload is too long.
  bool setBeacon(Uint8List payload);

  /// Sends one payload to a node id, [broadcastNode] for every node. True
  /// when the frame left on the link.
  bool send(int dst, Uint8List payload);

  /// Drains the link at [nowUs] (a monotonic instant of the caller's clock):
  /// learns nodes, queues the payloads for this node, expires the silent
  /// nodes, emits the beacon when due. Returns the payloads waiting.
  int poll(int nowUs);

  /// Takes the oldest received payload, null when none is waiting.
  InboundPayload? nextPayload();

  /// The live nodes, as of the last poll().
  List<NodeInfo> nodes();

  /// The counters.
  TransportStats stats();

  /// Closes the sockets.
  void dispose();
}
