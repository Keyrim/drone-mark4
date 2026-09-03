import 'dart:collection';
import 'dart:typed_data';

import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/gen/mark4.pb.dart';

/// A transport node in a test: the test is the network. It puts nodes in the
/// table and payloads in the queue; the manager polls them out.
class FakeTransportNode implements AbsTransportNode {
  FakeTransportNode(this.nodeId);

  @override
  final int nodeId;

  Uint8List? beacon;
  final List<(int dst, Uint8List payload)> sent = [];
  final Map<int, NodeInfo> table = {};
  final Queue<InboundPayload> queue = Queue();
  int polls = 0;
  int lastPollUs = 0;
  bool disposed = false;

  /// A node heard at [nowUs] from a default address.
  void hear(int id, int nowUs, {int received = 1}) {
    table[id] = NodeInfo(
      id: id,
      host: 0xC0A80401,
      port: 47821,
      lastSeenUs: nowUs,
      received: received,
      lost: 0,
      duplicates: 0,
    );
  }

  /// A node silent for too long: the transport forgot it.
  void forget(int id) => table.remove(id);

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
  bool setBeacon(Uint8List payload) {
    beacon = payload;
    return true;
  }

  @override
  bool send(int dst, Uint8List payload) {
    sent.add((dst, payload));
    return true;
  }

  @override
  int poll(int nowUs) {
    ++polls;
    lastPollUs = nowUs;
    return queue.length;
  }

  @override
  InboundPayload? nextPayload() => queue.isEmpty ? null : queue.removeFirst();

  @override
  List<NodeInfo> nodes() => table.values.toList();

  @override
  TransportStats stats() => TransportStats(sent: sent.length, dataPort: 47822);

  @override
  void dispose() {
    disposed = true;
  }
}

/// Bytes that are not an Envelope.
abstract final class InboundPayloadGarbage {
  static InboundPayload of(int src) => InboundPayload(
    src: src,
    bytes: Uint8List.fromList([0xFF, 0xFF, 0xFF, 0xFF, 0x01]),
  );
}
