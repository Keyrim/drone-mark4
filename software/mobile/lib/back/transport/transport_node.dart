import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/back/transport/udp_link.dart';

/// One node of the transport table, as this node keeps it.
class _Node {
  _Node({required this.id, required this.lastSeq});

  final int id;
  int host = 0;
  int port = 0;
  int lastSeenUs = 0;
  int lastSeq;
  int received = 0;
  int lost = 0;
  int duplicates = 0;
  int hops = 0;

  NodeInfo get info => NodeInfo(
    id: id,
    host: host,
    port: port,
    lastSeenUs: lastSeenUs,
    received: received,
    lost: lost,
    duplicates: duplicates,
    hops: hops,
  );
}

/// The phone as a node of the network: the transport of
/// `software/components/transport` in Dart, over one UDP link.
///
/// Every frame heard refreshes the node table (address, last sequence, hops,
/// counters); a node silent for [nodeExpiryUs] is forgotten. Presence is a
/// keepalive, a frame that is the header alone: broadcast every
/// [keepalivePeriodUs] and unicast once to a node the moment it first
/// appears, carrying nothing for the application. A frame repeating the last
/// sequence of its sender is a duplicate and is dropped. No relay: with one
/// link there is nowhere to forward to, so a frame for another node is only
/// used to learn its sender.
///
/// It reads no clock: every instant comes from the caller.
class TransportNode implements AbsTransportNode {
  TransportNode._(this._nodeId, this._link);

  /// Nodes remembered at once; a frame from a further one is dropped.
  static const int maxNodes = 32;

  /// Keepalive cadence [us].
  static const int keepalivePeriodUs = 1000000;

  /// Silence after which a node is forgotten [us]: three missed keepalives.
  static const int nodeExpiryUs = 3000000;

  /// A forward jump of the sequence larger than this is a sender that
  /// restarted, not a burst of losses, and counts as nothing.
  static const int resyncThreshold = 1024;

  /// Payloads kept between two polls; the oldest is dropped past that.
  static const int rxQueueSize = 64;

  /// Opens one node on [discoveryPort]; null when the sockets could not be
  /// opened or [nodeId] is 0.
  static Future<AbsTransportNode?> open(int nodeId, int discoveryPort) async {
    if (nodeId == broadcastNode) {
      return null;
    }
    final link = UdpLink(discoveryPort: discoveryPort);
    if (!await link.init()) {
      return null;
    }
    return TransportNode._(nodeId, link);
  }

  final int _nodeId;
  final UdpLink _link;
  final Map<int, _Node> _nodes = {};
  final Queue<InboundPayload> _rx = Queue();
  final StreamController<PresenceEvent> _presence =
      StreamController<PresenceEvent>.broadcast(sync: true);
  int _nextSeq = 0;
  int _lastKeepaliveUs = 0;
  bool _keepaliveSent = false;
  int _dropped = 0;
  int _sent = 0;
  int _sentBytes = 0;
  int _refused = 0;
  int _rxOverflow = 0;

  @override
  int get nodeId => _nodeId;

  @override
  Stream<PresenceEvent> get presence => _presence.stream;

  @override
  bool send(int dst, Uint8List payload) {
    if (payload.isEmpty || payload.length > maxPayload) {
      ++_refused;
      return false;
    }
    return _emit(dst, payload);
  }

  @override
  void poll(int nowUs) {
    for (
      var datagram = _link.receive();
      datagram != null;
      datagram = _link.receive()
    ) {
      _onFrame(datagram, nowUs);
    }
    _expire(nowUs);
    if (!_keepaliveSent || nowUs - _lastKeepaliveUs >= keepalivePeriodUs) {
      _lastKeepaliveUs = nowUs;
      _keepaliveSent = true;
      _sendKeepalive(broadcastNode);
    }
  }

  @override
  InboundPayload? nextPayload() => _rx.isEmpty ? null : _rx.removeFirst();

  @override
  List<NodeInfo> nodes() => [for (final node in _nodes.values) node.info];

  @override
  NodeInfo? findNode(int nodeId) => _nodes[nodeId]?.info;

  @override
  TransportStats stats() => TransportStats(
    sent: _sent,
    sentBytes: _sentBytes,
    refused: _refused,
    dropped: _dropped,
    rxOverflow: _rxOverflow,
    dataPort: _link.dataPort,
    loopbackFallback: _link.loopbackFallback,
  );

  @override
  void dispose() {
    _link.close();
    _nodes.clear();
    _rx.clear();
    unawaited(_presence.close());
  }

  /// One keepalive, a header alone: this node's presence, broadcast every
  /// [keepalivePeriodUs] and unicast once to a node the moment it first
  /// appears. Counted like any send.
  void _sendKeepalive(int dst) => _emit(dst, Uint8List(0));

  /// Frames one payload and hands it to the link.
  bool _emit(int dst, Uint8List payload) {
    final header = FrameHeader(src: _nodeId, dst: dst, seq: _nextSeq);
    _nextSeq = (_nextSeq + 1) & 0xFFFF;
    final frame = header.frame(payload);
    if (dst == broadcastNode) {
      return _countSend(_link.broadcast(frame), payload.length);
    }
    final target = _nodes[dst];
    if (target == null) {
      ++_dropped;
      ++_refused;
      return false;
    }
    return _countSend(
      _link.send(frame, target.host, target.port),
      payload.length,
    );
  }

  bool _countSend(bool ok, int size) {
    if (!ok) {
      ++_refused;
      return false;
    }
    ++_sent;
    _sentBytes += size;
    return true;
  }

  void _onFrame(UdpDatagram datagram, int nowUs) {
    final header = FrameHeader.decode(datagram.bytes);
    if (header == null) {
      ++_dropped;
      return;
    }
    if (header.src == _nodeId || header.src == broadcastNode) {
      // A broadcast comes back to its sender on a shared medium; it carries
      // nothing this node does not know.
      return;
    }
    final (node, isNew) = _learn(header, datagram, nowUs);
    if (node == null) {
      return;
    }
    if (isNew) {
      _presence.add(PresenceEvent(up: true, node: node.info));
      // The newcomer learns this node at once instead of waiting for the
      // next periodic keepalive.
      _sendKeepalive(node.id);
    }
    if (datagram.bytes.length > frameHeaderSize &&
        (header.dst == _nodeId || header.dst == broadcastNode)) {
      // A header alone is a keepalive: it was learnt from above and carries
      // nothing for the application.
      _queue(
        InboundPayload(
          src: header.src,
          bytes: Uint8List.sublistView(datagram.bytes, frameHeaderSize),
        ),
      );
    }
  }

  /// Refreshes or inserts the node a frame came from. The node is null when
  /// the frame must be dropped (a duplicate, or a full table), and the flag
  /// says whether the node was not known before.
  (_Node?, bool) _learn(FrameHeader header, UdpDatagram datagram, int nowUs) {
    var isNew = false;
    var node = _nodes[header.src];
    if (node == null) {
      if (_nodes.length >= maxNodes) {
        ++_dropped;
        return (null, false);
      }
      node = _Node(id: header.src, lastSeq: header.seq)..received = 1;
      _nodes[header.src] = node;
      isNew = true;
    } else {
      final delta = (header.seq - node.lastSeq) & 0xFFFF;
      if (delta == 0) {
        // The same frame again: a medium that duplicates, or this node's own
        // forwarding echoed back. Either way it was already handled.
        ++node.duplicates;
        return (null, false);
      }
      if (delta > 1 && delta < resyncThreshold) {
        node.lost += delta - 1;
      }
      node.lastSeq = header.seq;
      ++node.received;
    }
    node.host = datagram.host;
    node.port = datagram.port;
    node.lastSeenUs = nowUs;
    node.hops = header.hops;
    return (node, isNew);
  }

  /// Forgets every node silent for [nodeExpiryUs].
  void _expire(int nowUs) {
    final gone = [
      for (final node in _nodes.values)
        if (nowUs >= node.lastSeenUs && nowUs - node.lastSeenUs >= nodeExpiryUs)
          node,
    ];
    for (final node in gone) {
      _nodes.remove(node.id);
      _presence.add(PresenceEvent(up: false, node: node.info));
    }
  }

  void _queue(InboundPayload payload) {
    if (_rx.length >= rxQueueSize) {
      _rx.removeFirst();
      ++_rxOverflow;
    }
    _rx.add(payload);
  }
}
