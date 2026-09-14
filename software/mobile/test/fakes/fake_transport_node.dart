import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// A transport node in a test: the test is the network. It puts nodes in the
/// table and payloads in the queue; the messenger polls them out.
class FakeTransportNode implements AbsTransportNode {
  FakeTransportNode(this.nodeId);

  @override
  final int nodeId;

  final List<(int dst, Uint8List payload)> sent = [];
  final Map<int, NodeInfo> table = {};
  final Queue<InboundPayload> queue = Queue();
  final StreamController<PresenceEvent> _presence =
      StreamController<PresenceEvent>.broadcast(sync: true);
  int polls = 0;
  int lastPollUs = 0;
  bool disposed = false;

  @override
  Stream<PresenceEvent> get presence => _presence.stream;

  /// A node heard at [nowUs] from a default address.
  void hear(int id, int nowUs, {int received = 1}) {
    final known = table.containsKey(id);
    final node = NodeInfo(
      id: id,
      host: 0xC0A80401,
      port: 47821,
      lastSeenUs: nowUs,
      received: received,
      lost: 0,
      duplicates: 0,
      hops: 0,
    );
    table[id] = node;
    if (!known) {
      _presence.add(PresenceEvent(up: true, node: node));
    }
  }

  /// A node silent for too long: the transport forgot it.
  void forget(int id) {
    final node = table.remove(id);
    if (node != null) {
      _presence.add(PresenceEvent(up: false, node: node));
    }
  }

  /// One Envelope from [src], waiting in the queue.
  void receive(int src, Envelope envelope) =>
      queue.add(InboundPayload(src: src, bytes: envelope.writeToBuffer()));

  /// The Announce of a drone from [src], heard and queued at [nowUs].
  void announceDrone(
    int src,
    String name,
    NodeKind kind,
    int nowUs, {
    int wireHash = 0,
  }) {
    hear(src, nowUs);
    receive(
      src,
      Envelope()
        ..announce = (Announce()
          ..kind = kind
          ..name = name
          ..mcu = Mcu.SIM
          ..wireHash = wireHash),
    );
  }

  @override
  bool send(int dst, Uint8List payload) {
    sent.add((dst, payload));
    return true;
  }

  @override
  void poll(int nowUs) {
    ++polls;
    lastPollUs = nowUs;
  }

  @override
  InboundPayload? nextPayload() => queue.isEmpty ? null : queue.removeFirst();

  @override
  List<NodeInfo> nodes() => table.values.toList();

  @override
  NodeInfo? findNode(int nodeId) => table[nodeId];

  @override
  TransportStats stats() => TransportStats(sent: sent.length, dataPort: 47822);

  @override
  void dispose() {
    disposed = true;
    unawaited(_presence.close());
  }
}

/// Bytes that are not an Envelope.
abstract final class InboundPayloadGarbage {
  static InboundPayload of(int src) => InboundPayload(
    src: src,
    bytes: Uint8List.fromList([0xFF, 0xFF, 0xFF, 0xFF, 0x01]),
  );
}
