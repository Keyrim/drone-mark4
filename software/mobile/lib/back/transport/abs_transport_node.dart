import 'dart:typed_data';

import 'package:equatable/equatable.dart';
import 'package:mark4/back/transport/udp_link.dart';

/// One live node of the transport's table.
class NodeInfo extends Equatable {
  const NodeInfo({
    required this.id,
    required this.host,
    required this.port,
    required this.lastSeenUs,
    required this.received,
    required this.lost,
    required this.duplicates,
    required this.hops,
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

  /// Relays the last frame from it crossed; 0 for a direct neighbour.
  final int hops;

  /// Dotted IPv4 and port, `192.168.4.1:47821`.
  String get address => '${UdpLink.hostText(host)}:$port';

  @override
  List<Object?> get props => [
    id,
    host,
    port,
    lastSeenUs,
    received,
    lost,
    duplicates,
    hops,
  ];
}

/// Counters of the node and its link.
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

  /// Frames handed to the link, keepalives included.
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

/// A node appeared on the link, or was forgotten after its silence.
class PresenceEvent {
  const PresenceEvent({required this.up, required this.node});

  /// True when the node was heard for the first time, false when it expired.
  final bool up;

  /// The node, as the table holds it (as it was, for a node going down).
  final NodeInfo node;
}

/// One transport node with one UDP link: the Dart node, or a fake in tests.
/// Everything is non-blocking; [poll] is where frames flow, and the instant
/// comes from the caller because a node reads no clock.
abstract class AbsTransportNode {
  /// Identity of this node, never 0.
  int get nodeId;

  /// Every node appearing and expiring, told during [poll]. Listeners are
  /// called synchronously, so what they send leaves within the same poll.
  Stream<PresenceEvent> get presence;

  /// Sends one payload to a node id, [broadcastNode] for every node. True
  /// when the frame left on the link; an empty payload is refused.
  bool send(int dst, Uint8List payload);

  /// Drains the link at [nowUs] (a monotonic instant of the caller's clock):
  /// learns nodes, queues the payloads for this node, expires the silent
  /// nodes, emits the keepalive when due.
  void poll(int nowUs);

  /// Takes the oldest received payload, null when none is waiting.
  InboundPayload? nextPayload();

  /// The live nodes, as of the last poll().
  List<NodeInfo> nodes();

  /// One live node, null when unknown or expired.
  NodeInfo? findNode(int nodeId);

  /// The counters.
  TransportStats stats();

  /// Closes the link.
  void dispose();
}
