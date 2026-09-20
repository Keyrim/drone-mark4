import 'dart:async';
import 'dart:collection';
import 'dart:math';
import 'dart:typed_data';

import 'package:mark4/back/transport/abs_transport_node.dart';
import 'package:mark4/back/transport/frame.dart';
import 'package:mark4/back/transport/udp_link.dart';

/// One node of the transport table, as this node keeps it.
class _Node {
  _Node({required this.id});

  final int id;
  int host = 0;
  int port = 0;
  int lastSeenUs = 0;

  /// Next sequence of the unicast stream from this node to it.
  int txSeq = 0;

  /// Last sequence of the unicast stream from it to this node, meaningful
  /// once [unicastHeard].
  int unicastSeq = 0;
  bool unicastHeard = false;

  /// Last sequence of its broadcast stream, meaningful once
  /// [broadcastHeard].
  int broadcastSeq = 0;
  bool broadcastHeard = false;

  int received = 0;
  int lost = 0;
  int duplicates = 0;
  int hops = 0;

  /// Boot id of the node's current incarnation, 0 until its first
  /// keepalive.
  int boot = 0;

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
/// Every frame heard refreshes the node table (address, hops, counters); a
/// node silent for [nodeExpiryUs] is forgotten. A sequence numbers one
/// stream: the frames from one sender to one destination, the broadcast
/// being a stream of its own. A frame is therefore judged in the stream its
/// destination names, where a repeated sequence is a duplicate and is
/// dropped and a gap is a loss; a frame addressed to another node belongs
/// to no stream of this node and says its sender is alive, nothing more.
/// Presence is a keepalive, a flagged frame carrying this node's boot id:
/// broadcast every [keepalivePeriodUs] and unicast once to a node the
/// moment it first appears, carrying nothing for the application whatever
/// its size. A boot id that changes is another incarnation of the same node
/// id, reported as a node that went down and came back. No relay: with one
/// link there is nowhere to forward to, so a frame for another node is only
/// used to learn its sender.
///
/// It reads no clock: every instant comes from the caller.
class TransportNode implements AbsTransportNode {
  TransportNode._(this._nodeId, this._link) : _bootId = _drawBootId();

  /// Draws the identity of this run of this node. Never derived from the
  /// device: what it has to say is that the node restarted.
  static int _drawBootId() {
    final random = Random.secure();
    var drawn = 0;
    while (drawn == 0) {
      drawn = random.nextInt(1 << 32);
    }
    return drawn;
  }

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
  final int _bootId;
  final UdpLink _link;
  final Map<int, _Node> _nodes = {};
  final Queue<InboundPayload> _rx = Queue();
  final StreamController<PresenceEvent> _presence =
      StreamController<PresenceEvent>.broadcast(sync: true);
  int _broadcastSeq = 0;
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

  /// One keepalive, the flagged frame carrying this node's boot id: its
  /// presence, broadcast every [keepalivePeriodUs] and unicast once to a
  /// node the moment it first appears. Counted like any send, and its
  /// payload counts no byte: it is the transport's own, not a message.
  void _sendKeepalive(int dst) {
    final boot = Uint8List(keepalivePayloadSize);
    ByteData.sublistView(boot).setUint32(0, _bootId, Endian.little);
    _emit(dst, boot, keepalive: true);
  }

  /// Frames one payload and hands it to the link. The frame is numbered in
  /// the stream its destination names: the broadcast stream, or the unicast
  /// stream this node keeps towards that node.
  bool _emit(int dst, Uint8List payload, {bool keepalive = false}) {
    final counted = keepalive ? 0 : payload.length;
    if (dst == broadcastNode) {
      final frame = _frame(dst, _broadcastSeq, payload, keepalive);
      _broadcastSeq = (_broadcastSeq + 1) & 0xFFFF;
      return _countSend(_link.broadcast(frame), counted);
    }
    final target = _nodes[dst];
    if (target == null) {
      ++_dropped;
      ++_refused;
      return false;
    }
    final frame = _frame(dst, target.txSeq, payload, keepalive);
    target.txSeq = (target.txSeq + 1) & 0xFFFF;
    return _countSend(_link.send(frame, target.host, target.port), counted);
  }

  /// One whole frame from this node, header and payload.
  Uint8List _frame(int dst, int seq, Uint8List payload, bool keepalive) =>
      FrameHeader(
        src: _nodeId,
        dst: dst,
        seq: seq,
        keepalive: keepalive,
      ).frame(payload);

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
    var reincarnated = false;
    if (header.keepalive &&
        datagram.bytes.length >= frameHeaderSize + keepalivePayloadSize) {
      reincarnated = _onBootId(
        node,
        ByteData.sublistView(
          datagram.bytes,
        ).getUint32(frameHeaderSize, Endian.little),
        header,
        isNew,
      );
    }
    if (isNew) {
      _presence.add(PresenceEvent(up: true, node: node.info));
      // The newcomer learns this node at once instead of waiting for the
      // next periodic keepalive.
      _sendKeepalive(node.id);
    } else if (reincarnated) {
      _presence.add(PresenceEvent(up: false, node: node.info));
      _presence.add(PresenceEvent(up: true, node: node.info));
    }
    if (!header.keepalive &&
        datagram.bytes.length > frameHeaderSize &&
        (header.dst == _nodeId || header.dst == broadcastNode)) {
      // A flagged frame is the transport's own, whatever it carries, and a
      // frame without a payload has nothing for the application either:
      // both were learnt from above and stop here.
      _queue(
        InboundPayload(
          src: header.src,
          bytes: Uint8List.sublistView(datagram.bytes, frameHeaderSize),
        ),
      );
    }
  }

  /// Reads the boot id one keepalive carries. The first one is learnt
  /// silently; one that differs from a known incarnation is a node that
  /// restarted, whose counters and streams are reset and whose caller
  /// reports it as gone and back.
  bool _onBootId(_Node node, int boot, FrameHeader header, bool isNew) {
    if (isNew || node.boot == 0) {
      node.boot = boot;
      return false;
    }
    if (boot == 0 || boot == node.boot) {
      return false;
    }
    node.txSeq = 0;
    node.unicastSeq = 0;
    node.unicastHeard = false;
    node.broadcastSeq = 0;
    node.broadcastHeard = false;
    _takeSeq(node, header);
    node.received = 1;
    node.lost = 0;
    node.duplicates = 0;
    node.hops = header.hops;
    node.boot = boot;
    return true;
  }

  /// Refreshes or inserts the node a frame came from, and accounts for the
  /// frame in its stream. The node is null when the frame must be dropped (a
  /// duplicate, or a full table), and the flag says whether the node was not
  /// known before.
  (_Node?, bool) _learn(FrameHeader header, UdpDatagram datagram, int nowUs) {
    var isNew = false;
    var node = _nodes[header.src];
    if (node == null) {
      if (_nodes.length >= maxNodes) {
        ++_dropped;
        return (null, false);
      }
      node = _Node(id: header.src);
      _nodes[header.src] = node;
      isNew = true;
    }
    // A frame for another node carries the numbering of a stream this node
    // does not see: it says its sender is alive and nothing else.
    final mine = header.dst == _nodeId || header.dst == broadcastNode;
    if (mine && !_account(node, header)) {
      return (null, false);
    }
    node.host = datagram.host;
    node.port = datagram.port;
    node.lastSeenUs = nowUs;
    node.hops = header.hops;
    return (node, isNew);
  }

  /// Counts one frame in the stream its destination names. False when the
  /// frame must be dropped as a duplicate.
  bool _account(_Node node, FrameHeader header) {
    final broadcast = header.dst == broadcastNode;
    final heard = broadcast ? node.broadcastHeard : node.unicastHeard;
    if (heard) {
      final last = broadcast ? node.broadcastSeq : node.unicastSeq;
      final delta = (header.seq - last) & 0xFFFF;
      if (delta == 0) {
        // The same frame again: a medium that duplicates, or this node's own
        // forwarding echoed back. Either way it was already handled.
        ++node.duplicates;
        return false;
      }
      if (delta > 1 && delta < resyncThreshold) {
        node.lost += delta - 1;
      }
    }
    // The first frame of a stream opens its numbering and is judged on
    // nothing.
    _takeSeq(node, header);
    ++node.received;
    return true;
  }

  /// Takes the sequence of a frame into the stream its destination names,
  /// which the frame opens when it is its first.
  void _takeSeq(_Node node, FrameHeader header) {
    if (header.dst == broadcastNode) {
      node.broadcastSeq = header.seq;
      node.broadcastHeard = true;
    } else if (header.dst == _nodeId) {
      node.unicastSeq = header.seq;
      node.unicastHeard = true;
    }
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
